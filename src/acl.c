/*
 * Copyright (c) 2018, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "server.h"

/* =============================================================================
 * Global state for ACLs
 * ==========================================================================*/

rax *Users; /* Table mapping usernames to user structures. */
user *DefaultUser;   /* Global reference to the default user.
                        Every new connection is associated to it, if no
                        AUTH or HELLO is used to authenticate with a
                        different user. */

/* =============================================================================
 * Helper functions for the rest of the ACL implementation
 * ==========================================================================*/

/* Return zero if strings are the same, non-zero if they are not.
 * The comparison is performed in a way that prevents an attacker to obtain
 * information about the nature of the strings just monitoring the execution
 * time of the function.
 *
 * Note that limiting the comparison length to strings up to 512 bytes we
 * can avoid leaking any information about the password length and any
 * possible branch misprediction related leak.
 */
int time_independent_strcmp(char *a, char *b) {
    char bufa[CONFIG_AUTHPASS_MAX_LEN], bufb[CONFIG_AUTHPASS_MAX_LEN];
    /* The above two strlen perform len(a) + len(b) operations where either
     * a or b are fixed (our password) length, and the difference is only
     * relative to the length of the user provided string, so no information
     * leak is possible in the following two lines of code. */
    unsigned int alen = strlen(a);
    unsigned int blen = strlen(b);
    unsigned int j;
    int diff = 0;

    /* We can't compare strings longer than our static buffers.
     * Note that this will never pass the first test in practical circumstances
     * so there is no info leak. */
    if (alen > sizeof(bufa) || blen > sizeof(bufb)) return 1;

    memset(bufa,0,sizeof(bufa));        /* Constant time. */
    memset(bufb,0,sizeof(bufb));        /* Constant time. */
    /* Again the time of the following two copies is proportional to
     * len(a) + len(b) so no info is leaked. */
    memcpy(bufa,a,alen);
    memcpy(bufb,b,blen);

    /* Always compare all the chars in the two buffers without
     * conditional expressions. */
    for (j = 0; j < sizeof(bufa); j++) {
        diff |= (bufa[j] ^ bufb[j]);
    }
    /* Length must be equal as well. */
    diff |= alen ^ blen;
    return diff; /* If zero strings are the same. */
}

/* =============================================================================
 * Low level ACL API
 * ==========================================================================*/

/* Method for passwords/pattern comparison used for the user->passwords list
 * so that we can search for items with listSearchKey(). */
int ACLListMatchSds(void *a, void *b) {
    return sdscmp(a,b) == 0;
}

/* Create a new user with the specified name, store it in the list
 * of users (the Users global radix tree), and returns a reference to
 * the structure representing the user.
 *
 * If the user with such name already exists NULL is returned. */
user *ACLCreateUser(const char *name, size_t namelen) {
    if (raxFind(Users,(unsigned char*)name,namelen) != raxNotFound) return NULL;
    user *u = zmalloc(sizeof(*u));
    u->flags = 0;
    u->allowed_subcommands = NULL;
    u->passwords = listCreate();
    listSetMatchMethod(u->passwords,ACLListMatchSds);
    u->patterns = NULL; /* Just created users cannot access to any key, however
                           if the "~*" directive was enabled to match all the
                           keys, the user will be flagged with the ALLKEYS
                           flag. */
    memset(u->allowed_commands,0,sizeof(u->allowed_commands));
    raxInsert(Users,(unsigned char*)name,namelen,u,NULL);
    return u;
}

/* Set user properties according to the string "op". The following
 * is a description of what different strings will do:
 *
 * on           Enable the user
 * off          Disable the user
 * +<command>   Allow the execution of that command
 * -<command>   Disallow the execution of that command
 * +@<category> Allow the execution of all the commands in such category
 *              with valid categories being @set, @sortedset, @list, @hash,
 *                                          @string, @bitmap, @hyperloglog,
 *                                          @stream, @admin, @readonly,
 *                                          @readwrite, @fast, @slow,
 *                                          @pubsub.
 *              The special category @all means all the commands.
 * +<command>|subcommand    Allow a specific subcommand of an otherwise
 *                          disabled command. Note that this form is not
 *                          allowed as negative like -DEBUG|SEGFAULT, but
 *                          only additive starting with "+".
 * ~<pattern>   Add a pattern of keys that can be mentioned as part of
 *              commands. For instance ~* allows all the keys. The pattern
 *              is a glob-style pattern like the one of KEYS.
 *              It is possible to specify multiple patterns.
 * ><password>  Add this passowrd to the list of valid password for the user.
 *              For example >mypass will add "mypass" to the list.
 * <<password>  Remove this password from the list of valid passwords.
 * nopass       All the set passwords of the user are removed, and the user
 *              is flagged as requiring no password: it means that every
 *              password will work against this user. If this directive is
 *              used for the default user, every new connection will be
 *              immediately authenticated with the default user without
 *              any explicit AUTH command required. Note that the "resetpass"
 *              directive will clear this condition.
 * allcommands  Alias for +@all
 * allkeys      Alias for ~*
 * resetpass    Flush the list of allowed passwords. Moreover removes the
 *              "nopass" status. After "resetpass" the user has no associated
 *              passwords and there is no way to authenticate without adding
 *              some password (or setting it as "nopass" later).
 * resetkeys    Flush the list of allowed keys patterns.
 * reset        Performs the following actions: resetpass, resetkeys, off,
 *              -@all. The user returns to the same state it has immediately
 *              after its creation.
 *
 * The 'op' string must be null terminated. The 'oplen' argument should
 * specify the length of the 'op' string in case the caller requires to pass
 * binary data (for instance the >password form may use a binary password).
 * Otherwise the field can be set to -1 and the function will use strlen()
 * to determine the length.
 *
 * The function returns C_OK if the action to perform was understood because
 * the 'op' string made sense. Otherwise C_ERR is returned if the operation
 * is unknown or has some syntax error.
 */
int ACLSetUser(user *u, const char *op, ssize_t oplen) {
    if (oplen == -1) oplen = strlen(op);
    if (!strcasecmp(op,"on")) {
        u->flags |= USER_FLAG_ENABLED;
    } else if (!strcasecmp(op,"off")) {
        u->flags &= ~USER_FLAG_ENABLED;
    } else if (!strcasecmp(op,"allkeys") ||
               !strcasecmp(op,"~*"))
    {
        u->flags |= USER_FLAG_ALLKEYS;
        if (u->patterns) listEmpty(u->patterns);
    } else if (!strcasecmp(op,"allcommands") ||
               !strcasecmp(op,"+@all"))
    {
        memset(u->allowed_commands,255,sizeof(u->allowed_commands));
        u->flags |= USER_FLAG_ALLCOMMANDS;
    } else if (!strcasecmp(op,"nopass")) {
        u->flags |= USER_FLAG_NOPASS;
        listEmpty(u->passwords);
    } else if (op[0] == '>') {
        sds newpass = sdsnewlen(op+1,oplen-1);
        listNode *ln = listSearchKey(u->passwords,newpass);
        /* Avoid re-adding the same password multiple times. */
        if (ln == NULL) listAddNodeTail(u->passwords,newpass);
    } else if (op[0] == '<') {
        sds delpass = sdsnewlen(op+1,oplen-1);
        listNode *ln = listSearchKey(u->passwords,delpass);
        if (ln) listDelNode(u->passwords,ln);
        sdsfree(delpass);
    } else {
        return C_ERR;
    }
    return C_OK;
}

/* Initialization of the ACL subsystem. */
void ACLInit(void) {
    Users = raxNew();
    DefaultUser = ACLCreateUser("default",7);
    ACLSetUser(DefaultUser,"+@all",-1);
    ACLSetUser(DefaultUser,"on",-1);
    ACLSetUser(DefaultUser,"nopass",-1);
}

/* Check the username and password pair and return C_OK if they are valid,
 * otherwise C_ERR is returned and errno is set to:
 *
 *  EINVAL: if the username-password do not match.
 *  ENONENT: if the specified user does not exist at all.
 */
int ACLCheckUserCredentials(robj *username, robj *password) {
    /* For now only the "default" user is allowed. When the RCP1 ACLs
     * will be implemented multiple usernames will be supproted. */
    if (username != NULL && strcmp(username->ptr,"default")) {
        errno = ENOENT;
        return C_ERR;
    }

    /* For now we just compare the password with the system wide one. */
    if (!time_independent_strcmp(password->ptr, server.requirepass)) {
        return C_OK;
    } else {
        errno = EINVAL;
        return C_ERR;
    }
}

/* For ACL purposes, every user has a bitmap with the commands that such
 * user is allowed to execute. In order to populate the bitmap, every command
 * should have an assigned ID (that is used to index the bitmap). This function
 * creates such an ID: it uses sequential IDs, reusing the same ID for the same
 * command name, so that a command retains the same ID in case of modules that
 * are unloaded and later reloaded. */
unsigned long ACLGetCommandID(const char *cmdname) {
    static rax *map = NULL;
    static unsigned long nextid = 0;

    if (map == NULL) map = raxNew();
    void *id = raxFind(map,(unsigned char*)cmdname,strlen(cmdname));
    if (id != raxNotFound) return (unsigned long)id;
    raxInsert(map,(unsigned char*)cmdname,strlen(cmdname),(void*)nextid,NULL);
    return nextid++;
}

/* Return an username by its name, or NULL if the user does not exist. */
user *ACLGetUserByName(const char *name, size_t namelen) {
    void *myuser = raxFind(Users,(unsigned char*)name,namelen);
    if (myuser == raxNotFound) return NULL;
    return myuser;
}

/* Check if the command ready to be excuted in the client 'c', and already
 * referenced by c->cmd, can be executed by this client according to the
 * ACls associated to the client user c->user.
 *
 * If the user can execute the command C_OK is returned, otherwise
 * C_ERR is returned. */
int ACLCheckCommandPerm(client *c) {
    user *u = c->user;
    uint64_t id = c->cmd->id;

    /* If there is no associated user, the connection can run anything. */
    if (u == NULL) return C_OK;

    /* We have to deny every command with an ID that overflows the Redis
     * internal structures. Very unlikely to happen. */
    if (c->cmd->id >= USER_MAX_COMMAND_BIT) return C_ERR;

    /* Check if the user can execute this command. */
    if (!(u->flags & USER_FLAG_ALLCOMMANDS)) {
        uint64_t wordid = id / sizeof(u->allowed_commands[0]) / 8;
        uint64_t bit = 1 << (id % (sizeof(u->allowed_commands[0] * 8)));
        /* If the bit is not set we have to check further, in case the
         * command is allowed just with that specific subcommand. */
        if (!(u->allowed_commands[wordid] & bit)) {
            /* Check if the subcommand matches. */
            if (u->allowed_subcommands == NULL || c->argc < 2) return C_ERR;
            long subid = 0;
            while (1) {
                if (u->allowed_subcommands[id][subid] == NULL) return C_ERR;
                if (!strcasecmp(c->argv[1]->ptr,
                                u->allowed_subcommands[id][subid]))
                    break; /* Subcommand match found. Stop here. */
                subid++;
            }
        }
    }

    /* Check if the user can execute commands explicitly touching the keys
     * mentioned in the command arguments. */
    if (!(c->user->flags & USER_FLAG_ALLKEYS) &&
        (c->cmd->getkeys_proc || c->cmd->firstkey))
    {
    }

    /* If we survived all the above checks, the user can execute the
     * command. */
    return C_OK;
}

/* =============================================================================
 * ACL related commands
 * ==========================================================================*/

/* ACL -- show and modify the configuration of ACL users.
 * ACL help
 * ACL list
 * ACL setuser <username> ... user attribs ...
 * ACL deluser <username>
 * ACL getuser <username>
 */
void aclCommand(client *c) {
    char *sub = c->argv[1]->ptr;
    if (!strcasecmp(sub,"setuser") && c->argc >= 3) {
        sds username = c->argv[2]->ptr;
        user *u = ACLGetUserByName(username,sdslen(username));
        if (!u) u = ACLCreateUser(username,sdslen(username));
        serverAssert(u != NULL);
        for (int j = 3; j < c->argc; j++) {
            if (ACLSetUser(u,c->argv[j]->ptr,sdslen(c->argv[j]->ptr)) != C_OK) {
                addReplyErrorFormat(c,
                    "Syntax error in ACL SETUSER modifier '%s'",
                    c->argv[j]->ptr);
                return;
            }
        }
        addReply(c,shared.ok);
    } else if (!strcasecmp(sub,"help")) {
        const char *help[] = {
"LIST                              -- List all the registered users.",
"SETUSER <username> [attribs ...]  -- Create or modify a user.",
"DELUSER <username>                -- Delete a user.",
"GETUSER <username>                -- Get the user details.",
NULL
        };
        addReplyHelp(c,help);
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

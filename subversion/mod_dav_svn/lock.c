/*
 * lock.c: mod_dav_svn locking provider functions
 *
 * ====================================================================
 * Copyright (c) 2000-2004 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */



#include <httpd.h>
#include <http_log.h>
#include <mod_dav.h>
#include <apr_uuid.h>
#include <apr_time.h>

#include "svn_fs.h"
#include "svn_repos.h"
#include "svn_dav.h"
#include "svn_time.h"
#include "svn_pools.h"

#include "dav_svn.h"




struct dav_lockdb_private
{
  /* These represent 'custom' request hearders only sent by svn clients: */
  svn_boolean_t force;
  svn_revnum_t working_revnum;

  /* The original request, so we can set 'custom' output headers. */
  request_rec *r;
};


/* Helper func:  convert an svn_lock_t to a dav_lock, allocated in
   pool.  EXISTS_P indicates whether slock->path actually exists or not.
 */
static void
svn_lock_to_dav_lock(dav_lock **dlock,
                     const svn_lock_t *slock,
                     svn_boolean_t exists_p,
                     apr_pool_t *pool)
{
  dav_lock *lock = apr_pcalloc(pool, sizeof(*lock));
  dav_locktoken *token = apr_pcalloc(pool, sizeof(*token));

  lock->rectype = DAV_LOCKREC_DIRECT;
  lock->scope = DAV_LOCKSCOPE_EXCLUSIVE;
  lock->type = DAV_LOCKTYPE_WRITE;
  lock->depth = 0;
  lock->is_locknull = exists_p;

  token->uuid_str = apr_pstrdup(pool, slock->token);
  lock->locktoken = token;

  /* the svn_lock_t 'comment' is the equivalent of the 'DAV:owner'
     field, just a scratch-space for notes abotu the lock. */
  lock->owner = apr_pstrdup(pool, slock->comment);

  /* the svn_lock_t 'owner' is the actual authenticated owner of the lock. */
  lock->auth_user = apr_pstrdup(pool, slock->owner);

  /* ### This is absurd.  apr_time.h has an apr_time_t->time_t func,
     but not the reverse?? */
  if (slock->expiration_date)
    lock->timeout = (time_t)slock->expiration_date / APR_USEC_PER_SEC;
  else
    lock->timeout = DAV_TIMEOUT_INFINITE;

  /* ### uhoh.  There's no concept of a lock creation-time in DAV.
         How do we get that value over to the client?  Maybe we should
         just get rid of that field in svn_lock_t?  */

  *dlock = lock;
}



/* Helper func:  convert a dav_lock to an svn_lock_t, allocated in pool. */
static dav_error *
dav_lock_to_svn_lock(svn_lock_t **slock,
                     const dav_lock *dlock,
                     const char *path,
                     apr_pool_t *pool)
{
  svn_lock_t *lock;

  /* Sanity checks */
  if (dlock->type != DAV_LOCKTYPE_WRITE)
    return dav_new_error(pool, HTTP_BAD_REQUEST,
                         DAV_ERR_LOCK_SAVE_LOCK,
                         "Only 'write' locks are supported.");

  if (dlock->scope != DAV_LOCKSCOPE_EXCLUSIVE)
    return dav_new_error(pool, HTTP_BAD_REQUEST,
                         DAV_ERR_LOCK_SAVE_LOCK,
                         "Only exclusive locks are supported.");

  lock = apr_pcalloc(pool, sizeof(*lock));
  lock->path = apr_pstrdup(pool, path);
  lock->token = apr_pstrdup(pool, dlock->locktoken->uuid_str);

  /* DAV has no concept of lock creationdate, so assume 'now' */
  lock->creation_date = apr_time_now();

  if (dlock->auth_user)
    lock->owner = apr_pstrdup(pool, dlock->auth_user);
  
  if (dlock->owner)
    lock->comment = apr_pstrdup(pool, dlock->owner);

  if (dlock->timeout)
    lock->expiration_date = (apr_time_t)dlock->timeout * APR_USEC_PER_SEC;
  else
    lock->expiration_date = 0; /* never expires */

  *slock = lock;
  return 0;
}



/* Helper func:  invoke mod_dav_svn's authz_read callback on
   PATH in HEAD revision, return the readability result in *READABLE. */
static dav_error *
check_readability(svn_boolean_t *readable,
                  request_rec *r,
                  const dav_svn_repos *repos,
                  const char *path,
                  apr_pool_t *pool)
{
  svn_error_t *serr;
  svn_fs_root_t *headroot;
  svn_revnum_t headrev;
  dav_svn_authz_read_baton arb;

  arb.r = r;
  arb.repos = repos;

  serr = svn_fs_youngest_rev(&headrev, repos->fs, pool);
  if (serr)
    return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                               "Failed to get youngest filesystem revision.",
                               pool);

  serr = svn_fs_revision_root(&headroot, repos->fs, headrev, pool);
  if (serr)
    return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                               "Failed to open revision root for HEAD.",
                               pool);

  serr = dav_svn_authz_read(readable, headroot, path, &arb, pool);
  if (serr)
    return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                               "Failed to check readability of a path.",
                               pool);
  return 0;
}


/* ---------------------------------------------------------------- */
/* mod_dav locking vtable starts here: */


/* Return the supportedlock property for a resource */
static const char *
dav_svn_get_supportedlock(const dav_resource *resource)
{
  /* This is imitating what mod_dav_fs is doing.  Note that unlike
     mod_dav_fs, however, we don't support "shared" locks, only
     "exclusive" ones.  Nor do we support locks on collections. */
  static const char supported[] = DEBUG_CR
    "<D:lockentry>" DEBUG_CR
    "<D:lockscope><D:exclusive/></D:lockscope>" DEBUG_CR
    "<D:locktype><D:write/></D:locktype>" DEBUG_CR
    "</D:lockentry>" DEBUG_CR;

  if (resource->collection)
    return NULL;
  else
    return supported;
}



/* Parse a lock token URI, returning a lock token object allocated
 * in the given pool.
 */
static dav_error *
dav_svn_parse_locktoken(apr_pool_t *pool,
                        const char *char_token,
                        dav_locktoken **locktoken_p)
{
  dav_locktoken *token = apr_pcalloc(pool, sizeof(*token));
  
  /* Imitating mod_dav_fs again.  Hilariously, it also defines a
     locktoken just to be an apr uuid string!  */

  if (ap_strstr_c(char_token, "opaquelocktoken:") != char_token) 
    return dav_new_error(pool, HTTP_BAD_REQUEST,
                         DAV_ERR_LOCK_UNK_STATE_TOKEN,
                         "Client supplied lock token in unknown format.");

  char_token += 16;
  token->uuid_str = apr_pstrdup(pool, char_token);
  
  *locktoken_p = token;
  return 0;
}



/* Format a lock token object into a URI string, allocated in
 * the given pool.
 *
 * Always returns non-NULL.
 */
static const char *
dav_svn_format_locktoken(apr_pool_t *p,
                         const dav_locktoken *locktoken)
{
  /* Imitating mod_dav_fs again.  Hilariously, it also defines a
     locktoken just to be an apr uuid string!  */

  return apr_pstrcat(p, "opaquelocktoken:", locktoken->uuid_str, NULL);
}



/* Compare two lock tokens.
 *
 * Result < 0  => lt1 < lt2
 * Result == 0 => lt1 == lt2
 * Result > 0  => lt1 > lt2
 */
static int
dav_svn_compare_locktoken(const dav_locktoken *lt1,
                          const dav_locktoken *lt2)
{
  return strcmp(lt1->uuid_str, lt2->uuid_str);
}



/* Open the provider's lock database.
 *
 * The provider may or may not use a "real" database for locks
 * (a lock could be an attribute on a resource, for example).
 *
 * The provider may choose to use the value of the DAVLockDB directive
 * (as returned by dav_get_lockdb_path()) to decide where to place
 * any storage it may need.
 *
 * The request storage pool should be associated with the lockdb,
 * so it can be used in subsequent operations.
 *
 * If ro != 0, only readonly operations will be performed.
 * If force == 0, the open can be "lazy"; no subsequent locking operations
 * may occur.
 * If force != 0, locking operations will definitely occur.
 */
static dav_error *
dav_svn_open_lockdb(request_rec *r,
                    int ro,
                    int force,
                    dav_lockdb **lockdb)
{
  const char *svn_client_options, *version_name;
  dav_lockdb *db = apr_pcalloc(r->pool, sizeof(*db));
  dav_lockdb_private *info = apr_pcalloc(r->pool, sizeof(*info));

  info->r = r;

  /* Check to see if an svn client sent any custom X-SVN-* headers in
     the request. */
  svn_client_options = apr_table_get(r->headers_in, SVN_DAV_OPTIONS_HEADER);
  if (svn_client_options)
    {
      /* 'svn [lock | unlock] --force' */
      if (ap_strstr_c(svn_client_options, SVN_DAV_OPTION_FORCE))
        info->force = TRUE;
    }

  /* 'svn lock' wants to make svn_fs_lock() do an out-of-dateness check. */
  version_name = apr_table_get(r->headers_in, SVN_DAV_VERSION_NAME_HEADER);
  info->working_revnum = version_name ? 
                         SVN_STR_TO_REV(version_name): SVN_INVALID_REVNUM;

  /* The generic lockdb structure.  */
  db->hooks = &dav_svn_hooks_locks;
  db->ro = ro;
  db->info = info;

  *lockdb = db;
  return 0;
}



/* Indicates completion of locking operations */
static void
dav_svn_close_lockdb(dav_lockdb *lockdb)
{
  /* nothing to do here. */
  return;
}



/* Take a resource out of the lock-null state. */
static dav_error *
dav_svn_remove_locknull_state(dav_lockdb *lockdb,
                              const dav_resource *resource)
{
  /* ### perhaps our resource->info context should keep track if a
     resource is in 'locknull' state', and not merely non-existent?
     According to RFC 2518, 'locknull' resources are supposed to be
     listed as children of their parent collections (e.g. a PROPFIND
     on the parent).  */

  return 0;  /* temporary: just to suppress compile warnings */
}



/*
** Create a (direct) lock structure for the given resource. A locktoken
** will be created.
**
** The lock provider may store private information into lock->info.
*/
static dav_error *
dav_svn_create_lock(dav_lockdb *lockdb,
                    const dav_resource *resource,
                    dav_lock **lock)
{
  svn_error_t *serr;
  dav_locktoken *token = apr_pcalloc(resource->pool, sizeof(*token));
  dav_lock *dlock = apr_pcalloc(resource->pool, sizeof(*dlock));
  
  dlock->rectype = DAV_LOCKREC_DIRECT;
  dlock->is_locknull = resource->exists;
  dlock->scope = DAV_LOCKSCOPE_UNKNOWN;
  dlock->type = DAV_LOCKTYPE_UNKNOWN;
  dlock->depth = 0;

  serr = svn_fs_generate_token(&(token->uuid_str), 
                               resource->info->repos->fs,
                               resource->pool);
  if (serr)
    return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                               "Failed to generate a lock token.",
                               resource->pool);
  dlock->locktoken = token;

  /* allowing mod_dav to fill in dlock->timeout, owner, auth_user. */
  /* dlock->info and dlock->next are NULL by default. */

  *lock = dlock;
  return 0;  
}



/*
** Get the locks associated with the specified resource.
**
** If resolve_locks is true (non-zero), then any indirect locks are
** resolved to their actual, direct lock (i.e. the reference to followed
** to the original lock).
**
** The locks, if any, are returned as a linked list in no particular
** order. If no locks are present, then *locks will be NULL.
**
** #define DAV_GETLOCKS_RESOLVED   0    -- resolve indirects to directs 
** #define DAV_GETLOCKS_PARTIAL    1    -- leave indirects partially filled 
** #define DAV_GETLOCKS_COMPLETE   2    -- fill out indirect locks
*/
static dav_error *
dav_svn_get_locks(dav_lockdb *lockdb,
                  const dav_resource *resource,
                  int calltype,
                  dav_lock **locks)
{
  dav_lockdb_private *info = lockdb->info;
  svn_error_t *serr;
  dav_error *derr;
  svn_lock_t *slock;
  svn_boolean_t readable = FALSE;
  dav_lock *lock = NULL;

  /* We only support exclusive locks, not shared ones.  So this
     function always returns a "list" of exactly one lock, or just a
     NULL list.  The 'calltype' arg is also meaningless, since we
     don't support locks on collections.  */
  
  /* Sanity check:  if the resource has no associated path in the fs,
     then there's nothing to do.  */
  if (! resource->info->repos_path)
    {
      *locks = NULL;
      return 0;
    }

  /* The Big Lie:  if an svn client passed a 'force' flag to 'svn
     lock', then we want to pretend that there's no existing lock no
     matter what.  Otherwise mod_dav will throw '403 Locked' without
     even attempting to create a new lock.  */
  if (info->force)
    {
      *locks = NULL;
      return 0;
    }

  /* If the resource's fs path is unreadable, we don't want to say
     anything about locks attached to it.*/
  derr = check_readability(&readable,
                           resource->info->r, resource->info->repos,
                           resource->info->repos_path, resource->pool);
  if (derr)
    return derr;
  if (! readable)
    return dav_new_error(resource->pool, HTTP_FORBIDDEN,
                         DAV_ERR_LOCK_SAVE_LOCK,
                         "Path is not accessible.");

  serr = svn_fs_get_lock_from_path(&slock,
                                   resource->info->repos->fs,
                                   resource->info->repos_path,
                                   resource->pool);
  if (serr)
    return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                               "Failed to check path for a lock.",
                               resource->pool);

  if (slock != NULL)
    svn_lock_to_dav_lock(&lock, slock, resource->exists, resource->pool);

  *locks = lock;
  return 0;  
}



/*
** Find a particular lock on a resource (specified by its locktoken).
**
** *lock will be set to NULL if the lock is not found.
**
** Note that the provider can optimize the unmarshalling -- only one
** lock (or none) must be constructed and returned.
**
** If partial_ok is true (non-zero), then an indirect lock can be
** partially filled in. Otherwise, another lookup is done and the
** lock structure will be filled out as a DAV_LOCKREC_INDIRECT.
*/
static dav_error *
dav_svn_find_lock(dav_lockdb *lockdb,
                  const dav_resource *resource,
                  const dav_locktoken *locktoken,
                  int partial_ok,
                  dav_lock **lock)
{
  svn_error_t *serr;
  dav_error *derr;
  svn_lock_t *slock;
  dav_lock *dlock;
  svn_boolean_t readable = FALSE;
  
  /* If the resource's fs path is unreadable, we don't want to say
     anything about locks attached to it.*/
  derr = check_readability(&readable,
                           resource->info->r, resource->info->repos,
                           resource->info->repos_path, resource->pool);
  if (derr)
    return derr;
  if (! readable)
    return dav_new_error(resource->pool, HTTP_FORBIDDEN,
                         DAV_ERR_LOCK_SAVE_LOCK,
                         "Path is not accessible.");

  serr = svn_fs_get_lock_from_token(&slock,
                                    resource->info->repos->fs,
                                    locktoken->uuid_str,
                                    resource->pool);
  if (serr &&
      ((serr->apr_err == SVN_ERR_FS_BAD_LOCK_TOKEN)
       || (serr->apr_err == SVN_ERR_FS_LOCK_EXPIRED)))
    dlock = NULL;
  else if (serr)
    return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                               "Failed to lookup lock via token.",
                               resource->pool);

  if (slock != NULL)
    svn_lock_to_dav_lock(&dlock, slock, resource->exists, resource->pool);

  *lock = dlock;
  return 0;  
}



/*
** Quick test to see if the resource has *any* locks on it.
**
** This is typically used to determine if a non-existent resource
** has a lock and is (therefore) a locknull resource.
**
** WARNING: this function may return TRUE even when timed-out locks
**          exist (i.e. it may not perform timeout checks).
*/
static dav_error *
dav_svn_has_locks(dav_lockdb *lockdb,
                  const dav_resource *resource,
                  int *locks_present)
{
  dav_lockdb_private *info = lockdb->info;
  svn_error_t *serr;
  dav_error *derr;
  svn_lock_t *slock;
  svn_boolean_t readable = FALSE;

  /* Sanity check:  if the resource has no associated path in the fs,
     then there's nothing to do.  */
  if (! resource->info->repos_path)
    {
      *locks_present = 0;
      return 0;
    }

  /* The Big Lie:  if an svn client passed a 'force' flag to 'svn
     lock', then we want to pretend that there's no existing lock no
     matter what.  Otherwise mod_dav will throw '403 Locked' without
     even attempting to create a new lock.  */
  if (info->force)
    {
      *locks_present = 0;
      return 0;
    }

  /* If the resource's fs path is unreadable, we don't want to say
     anything about locks attached to it.*/
  derr = check_readability(&readable,
                           resource->info->r, resource->info->repos,
                           resource->info->repos_path, resource->pool);
  if (derr)
    return derr;
  if (! readable)
    return dav_new_error(resource->pool, HTTP_FORBIDDEN,
                         DAV_ERR_LOCK_SAVE_LOCK,
                         "Path is not accessible.");

  serr = svn_fs_get_lock_from_path(&slock,
                                   resource->info->repos->fs,
                                   resource->info->repos_path,
                                   resource->pool);
  if (serr)
    return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                               "Failed to check path for a lock.",
                               resource->pool);

  *locks_present = slock ? 1 : 0;
  return 0;
}



/*
** Append the specified lock(s) to the set of locks on this resource.
**
** If "make_indirect" is true (non-zero), then the specified lock(s)
** should be converted to an indirect lock (if it is a direct lock)
** before appending. Note that the conversion to an indirect lock does
** not alter the passed-in lock -- the change is internal the
** append_locks function.
**
** Multiple locks are specified using the lock->next links.
*/
static dav_error *
dav_svn_append_locks(dav_lockdb *lockdb,
                     const dav_resource *resource,
                     int make_indirect,
                     const dav_lock *lock)
{
  dav_lockdb_private *info = lockdb->info;
  svn_lock_t *slock;
  svn_error_t *serr;
  dav_error *derr;
  svn_boolean_t readable = FALSE;

  /* If the resource's fs path is unreadable, we don't allow a lock to
     be created on it. */
  derr = check_readability(&readable,
                           resource->info->r, resource->info->repos,
                           resource->info->repos_path, resource->pool);
  if (derr)
    return derr;
  if (! readable)
    return dav_new_error(resource->pool, HTTP_FORBIDDEN,
                         DAV_ERR_LOCK_SAVE_LOCK,
                         "Path is not accessible.");

  if (lock->next)
    return dav_new_error(resource->pool, HTTP_BAD_REQUEST,
                         DAV_ERR_LOCK_SAVE_LOCK,
                         "Tried to attach multiple locks to a resource.");

  /* Convert the dav_lock into an svn_lock_t. */  
  derr = dav_lock_to_svn_lock(&slock, lock, resource->info->repos_path,
                              resource->pool);
  if (derr)
    return derr;

  /* Now use the svn_lock_t to actually perform the lock. */
  serr = svn_repos_fs_attach_lock(slock,
                                  resource->info->repos->repos,
                                  info->force,
                                  info->working_revnum,
                                  resource->pool);

  if (serr && serr->apr_err == SVN_ERR_FS_NO_USER)
    return dav_new_error(resource->pool, HTTP_UNAUTHORIZED,
                         DAV_ERR_LOCK_SAVE_LOCK,
                         "Anonymous lock creation is not allowed.");    
  else if (serr)
    return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                               "Failed to create new lock.",
                               resource->pool);


  /* A standard webdav LOCK response doesn't include any information
     about the creation date.  We send it in a custom header, so that
     svn clients can fill in svn_lock_t->creation_date.  A generic DAV
     client should just ignore the header. */
  apr_table_setn(info->r->headers_out, SVN_DAV_CREATIONDATE_HEADER,
                 svn_time_to_cstring (slock->creation_date, resource->pool));

  return 0;
}



/*
** Remove any lock that has the specified locktoken.
**
** If locktoken == NULL, then ALL locks are removed.
*/
static dav_error *
dav_svn_remove_lock(dav_lockdb *lockdb,
                    const dav_resource *resource,
                    const dav_locktoken *locktoken)
{
  dav_lockdb_private *info = lockdb->info;
  svn_error_t *serr;
  dav_error *derr;
  svn_boolean_t readable = FALSE;
  svn_lock_t *slock;
  const char *token = NULL;

  /* Sanity check:  if the resource has no associated path in the fs,
     then there's nothing to do.  */
  if (! resource->info->repos_path)
    return 0;

  /* If the resource's fs path is unreadable, we don't allow a lock to
     be removed from it. */
  derr = check_readability(&readable,
                           resource->info->r, resource->info->repos,
                           resource->info->repos_path, resource->pool);
  if (derr)
    return derr;
  if (! readable)
    return dav_new_error(resource->pool, HTTP_FORBIDDEN,
                         DAV_ERR_LOCK_SAVE_LOCK,
                         "Path is not accessible.");

  if (locktoken == NULL)
    {
      /* Need to manually discover any lock on the resource. */     
      serr = svn_fs_get_lock_from_path(&slock,
                                       resource->info->repos->fs,
                                       resource->info->repos_path,
                                       resource->pool);
      if (serr)
        return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                   "Failed to check path for a lock.",
                                   resource->pool);
      if (slock)
        token = slock->token;
    }
  else
    {
      token = locktoken->uuid_str;
    }

  if (token)
    {
      /* Notice that a generic DAV client is unable to forcibly
         'break' a lock, because info->force will always be FALSE.  An
         svn client, however, can request a 'forced' break.*/
      serr = svn_repos_fs_unlock(resource->info->repos->repos,
                                 token,
                                 info->force,
                                 resource->pool);

      if (serr && serr->apr_err == SVN_ERR_FS_NO_USER)
        return dav_new_error(resource->pool, HTTP_UNAUTHORIZED,
                             DAV_ERR_LOCK_SAVE_LOCK,
                             "Anonymous lock removal is not allowed.");
      else if (serr)
        return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                                   "Failed to remove a lock.",
                                   resource->pool);
    }

  return 0;
}



/*
** Refresh all locks, found on the specified resource, which has a
** locktoken in the provided list.
**
** If the lock is indirect, then the direct lock is referenced and
** refreshed.
**
** Each lock that is updated is returned in the <locks> argument.
** Note that the locks will be fully resolved.
*/
static dav_error *
dav_svn_refresh_locks(dav_lockdb *lockdb,
                      const dav_resource *resource,
                      const dav_locktoken_list *ltl,
                      time_t new_time,
                      dav_lock **locks)
{
  /* We're not looping over a list of locks, since we only support one
     lock per resource. */
  dav_locktoken *token = ltl->locktoken;
  svn_error_t *serr;
  dav_error *derr;
  svn_lock_t *slock;
  dav_lock *dlock;
  svn_boolean_t readable = FALSE;

  /* If the resource's fs path is unreadable, we don't want to say
     anything about locks attached to it.*/
  derr = check_readability(&readable,
                           resource->info->r, resource->info->repos,
                           resource->info->repos_path, resource->pool);
  if (derr)
    return derr;
  if (! readable)
    return dav_new_error(resource->pool, HTTP_FORBIDDEN,
                         DAV_ERR_LOCK_SAVE_LOCK,
                         "Path is not accessible.");

  /* Convert the token into an svn_lock_t. */
  serr = svn_fs_get_lock_from_token(&slock,
                                    resource->info->repos->fs,
                                    token->uuid_str,
                                    resource->pool);
  if (serr)
    return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                               "Token doesn't point to a lock.",
                               resource->pool);

  /* Sanity check: does the incoming token actually represent the
     current lock on the incoming resource? */
  if ((! resource->info->repos_path)
      || (strcmp(resource->info->repos_path, slock->path) != 0))
    return dav_new_error(resource->pool, HTTP_UNAUTHORIZED,
                         DAV_ERR_LOCK_SAVE_LOCK,
                         "Lock refresh request doesn't match existing lock.");

  /* Tweak the expiration_date to the new expiration time. */
  slock->expiration_date = (apr_time_t)new_time * APR_USEC_PER_SEC;

  /* Now use the tweaked svn_lock_t to 'refresh' the existing lock. */
  serr = svn_repos_fs_attach_lock(slock,
                                  resource->info->repos->repos,
                                  TRUE, /* forcibly steal existing lock */
                                  SVN_INVALID_REVNUM,
                                  resource->pool);

  if (serr && serr->apr_err == SVN_ERR_FS_NO_USER)
    return dav_new_error(resource->pool, HTTP_UNAUTHORIZED,
                         DAV_ERR_LOCK_SAVE_LOCK,
                         "Anonymous lock refreshing is not allowed.");    
  else if (serr)
    return dav_svn_convert_err(serr, HTTP_INTERNAL_SERVER_ERROR,
                               "Failed to refresh existing lock.",
                               resource->pool);

  /* Convert the refreshed lock into a dav_lock and return it. */
  svn_lock_to_dav_lock(&dlock, slock, resource->exists, resource->pool);
  *locks = dlock;

  return 0;
}




/* The main locking vtable, provided to mod_dav */

const dav_hooks_locks dav_svn_hooks_locks = {
  dav_svn_get_supportedlock,
  dav_svn_parse_locktoken,
  dav_svn_format_locktoken,
  dav_svn_compare_locktoken,
  dav_svn_open_lockdb,
  dav_svn_close_lockdb,
  dav_svn_remove_locknull_state,
  dav_svn_create_lock,
  dav_svn_get_locks,
  dav_svn_find_lock,
  dav_svn_has_locks,
  dav_svn_append_locks,
  dav_svn_remove_lock,
  dav_svn_refresh_locks,
  NULL,
  NULL                          /* hook structure context */
};

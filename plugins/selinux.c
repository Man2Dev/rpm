#include "system.h"

#include <selinux/selinux.h>
#include <selinux/context.h>
#include <selinux/flask.h>
#include <selinux/label.h>
#include <selinux/avc.h>
#include <rpm/rpmlog.h>
#include <rpm/rpmts.h>
#include "plugin.h"

#include "debug.h"

rpmPluginHook PLUGIN_HOOKS = (PLUGINHOOK_INIT|PLUGINHOOK_CLEANUP|
			      PLUGINHOOK_TSM_PRE|PLUGINHOOK_TSM_POST|
			      PLUGINHOOK_PSM_PRE|
			      PLUGINHOOK_SCRIPTLET_FORK_POST|
			      PLUGINHOOK_FSM_FILE_PREPARE);

static struct selabel_handle * sehandle = NULL;

static void sehandle_fini(int close_status)
{
    if (sehandle) {
	selabel_close(sehandle);
	sehandle = NULL;
    }
    if (close_status) {
	selinux_status_close();
    }
}

static rpmRC sehandle_init(int open_status)
{
    const char * path = selinux_file_context_path();
    struct selinux_opt opts[] = {
	{ .type = SELABEL_OPT_PATH, .value = path }
    };
    
    if (path == NULL)
	return RPMRC_FAIL;

    if (open_status) {
	selinux_status_close();
	if (selinux_status_open(0) < 0) {
	    return RPMRC_FAIL;
	}
    } else if (!selinux_status_updated() && sehandle) {
	return RPMRC_OK;
    }

    if (sehandle)
	sehandle_fini(0);

    sehandle = selabel_open(SELABEL_CTX_FILE, opts, 1);

    return (sehandle != NULL) ? RPMRC_OK : RPMRC_FAIL;
}

rpmRC PLUGINHOOK_INIT_FUNC(rpmts ts, const char *name, const char *opts)
{
    return RPMRC_OK;
}

rpmRC PLUGINHOOK_CLEANUP_FUNC(void)
{
    return RPMRC_OK;
}

rpmRC PLUGINHOOK_TSM_PRE_FUNC(rpmts ts)
{
    rpmRC rc = RPMRC_OK;

    /* If SELinux isn't enabled on the system, dont mess with it */
    if (!is_selinux_enabled()) {
	rpmtsSetFlags(ts, (rpmtsFlags(ts) | RPMTRANS_FLAG_NOCONTEXTS));
    }

    /* If not enabled or a test-transaction, dont bother with labels */
    if (!(rpmtsFlags(ts) & (RPMTRANS_FLAG_NOCONTEXTS|RPMTRANS_FLAG_TEST))) {
	rc = sehandle_init(1);
    }

    return rc;
}

rpmRC PLUGINHOOK_TSM_POST_FUNC(rpmts ts, int rc)
{
    if (sehandle) {
	sehandle_fini(1);
    }
    return RPMRC_OK;
}

rpmRC PLUGINHOOK_PSM_PRE_FUNC(rpmte te)
{
    rpmRC rc = RPMRC_OK;

    if (sehandle) {
	/* reload the labels if policy changed underneath */
	rc = sehandle_init(0);
    }
    return rc;
}

rpmRC PLUGINHOOK_SCRIPTLET_FORK_POST_FUNC(const char *path, int type)
{
    rpmRC rc = RPMRC_FAIL;
    security_context_t mycon = NULL, fcon = NULL, newcon = NULL;
    context_t con = NULL;
    int xx;

    if (sehandle == NULL)
	return RPMRC_OK;

    /* Figure the context to for next exec() */
    if (getcon(&mycon) < 0)
	goto exit;
    if (getfilecon(path, &fcon) < 0)
	goto exit;
    if (security_compute_create(mycon, fcon, SECCLASS_PROCESS, &newcon) < 0)
	goto exit;

    if (rstreq(mycon, newcon)) {
	/* No default transition, use rpm_script_t for now. */
	const char * script_type = "rpm_script_t";

	con = context_new(mycon);
	if (!con)
	    goto exit;
	if (context_type_set(con, script_type))
	    goto exit;
	freecon(newcon);
	newcon = xstrdup(context_str(con));
    }

    if ((xx = setexeccon(newcon)) == 0)
	rc = RPMRC_OK;

    if (rpmIsDebug()) {
	rpmlog(RPMLOG_DEBUG, "setexeccon: (%s, %s) %s\n",
	       path, newcon, (xx < 0 ? strerror(errno) : ""));
    }

exit:
    context_free(con);
    freecon(newcon);
    freecon(fcon);
    freecon(mycon);

    return rc;
}

rpmRC PLUGINHOOK_FSM_FILE_PREPARE_FUNC(const char *path, const char *dest,
				       mode_t file_mode, rpmFsmOp op)
{
    rpmRC rc = RPMRC_FAIL; /* assume failure */
    rpmFileAction action = XFO_ACTION(op);

    if (!XFA_SKIPPING(action)) {
	security_context_t scon = NULL;
	if (selabel_lookup_raw(sehandle, &scon, dest, file_mode) == 0) {
	    int conrc = lsetfilecon(path, scon);

	    if (rpmIsDebug()) {
		rpmlog(RPMLOG_DEBUG, "lsetfilecon: (%s, %s) %s\n",
		       path, scon, (rc < 0 ? strerror(errno) : ""));
	    }

	    if (conrc == 0 || (conrc < 0 && errno == EOPNOTSUPP))
		rc = RPMRC_OK;
	}
	freecon(scon);
    } else {
	rc = RPMRC_OK;
    }

    return rc;
}

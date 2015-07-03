/*
 * (c) Copyright 2011-2014, Hewlett-Packard Development Company, LP
 */

/* -*- mode:C++; c-basic-offset:4 -*-
     Shore-MT -- Multi-threaded port of the SHORE storage manager

                       Copyright (c) 2007-2009
      Data Intensive Applications and Systems Labaratory (DIAS)
               Ecole Polytechnique Federale de Lausanne

                         All Rights Reserved.

   Permission to use, copy, modify and distribute this software and
   its documentation is hereby granted, provided that both the
   copyright notice and this permission notice appear in all copies of
   the software, derivative works or modified versions, and any
   portions thereof, and that both notices appear in supporting
   documentation.

   This code is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. THE AUTHORS
   DISCLAIM ANY LIABILITY OF ANY KIND FOR ANY DAMAGES WHATSOEVER
   RESULTING FROM THE USE OF THIS SOFTWARE.
*/

/*<std-header orig-src='shore'>

 $Id: sm.cpp,v 1.501 2010/12/17 19:36:26 nhall Exp $

SHORE -- Scalable Heterogeneous Object REpository

Copyright (c) 1994-99 Computer Sciences Department, University of
                      Wisconsin -- Madison
All Rights Reserved.

Permission to use, copy, modify and distribute this software and its
documentation is hereby granted, provided that both the copyright
notice and this permission notice appear in all copies of the
software, derivative works or modified versions, and any portions
thereof, and that both notices appear in supporting documentation.

THE AUTHORS AND THE COMPUTER SCIENCES DEPARTMENT OF THE UNIVERSITY
OF WISCONSIN - MADISON ALLOW FREE USE OF THIS SOFTWARE IN ITS
"AS IS" CONDITION, AND THEY DISCLAIM ANY LIABILITY OF ANY KIND
FOR ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.

This software was developed with support by the Advanced Research
Project Agency, ARPA order number 018 (formerly 8230), monitored by
the U.S. Army Research Laboratory under contract DAAB07-91-C-Q518.
Further funding for this work was provided by DARPA through
Rome Research Laboratory Contract No. F30602-97-2-0247.

*/

#include "w_defines.h"

/*  -- do not edit anything above this line --   </std-header>*/

#define SM_SOURCE
#define SM_C

#ifdef __GNUG__
class prologue_rc_t;
#endif

#include "w.h"
#include "sm_base.h"
#include "chkpt.h"
#include "chkpt_serial.h"
#include "sm.h"
#include "sm_vtable_enum.h"
#include "prologue.h"
#include "vol.h"
#include "bf_tree.h"
#include "crash.h"
#include "restart.h"
#include "sm_options.h"
#include "suppress_unused.h"
#include "backup.h"
#include "tid_t.h"
#include "log_carray.h"
#include "log_lsn_tracker.h"

#include "allocator.h"
#include "plog_xct.h"
#include "logbuf_common.h"
#include "log_core.h"
#include "logbuf_core.h"

#include <netdb.h> // CS: for generate_new_lvid


#ifdef EXPLICIT_TEMPLATE
template class w_auto_delete_t<SmStoreMetaStats*>;
#endif

bool         smlevel_0::shutdown_clean = true;
bool         smlevel_0::shutting_down = false;

smlevel_0::operating_mode_t
            smlevel_0::operating_mode = smlevel_0::t_not_started;

lsn_t        smlevel_0::commit_lsn = lsn_t::null;
lsn_t        smlevel_0::redo_lsn = lsn_t::null;
lsn_t        smlevel_0::last_lsn = lsn_t::null;
uint32_t     smlevel_0::in_doubt_count = 0;

#ifdef USE_TLS_ALLOCATOR
    sm_tls_allocator smlevel_0::allocator;
#else
    sm_naive_allocator smlevel_0::allocator;
#endif


// This is the controlling variable to determine which mode to use at run time if user did not specify restart mode:
smlevel_0::restart_internal_mode_t
           smlevel_0::restart_internal_mode =
                 (smlevel_0::restart_internal_mode_t)m1_default_restart;


            //controlled by AutoTurnOffLogging:
bool        smlevel_0::lock_caching_default = true;
bool        smlevel_0::logging_enabled = true;
bool        smlevel_0::do_prefetch = false;

bool        smlevel_0::statistics_enabled = true;

#ifndef SM_LOG_WARN_EXCEED_PERCENT
#define SM_LOG_WARN_EXCEED_PERCENT 40
#endif
smlevel_0::fileoff_t smlevel_0::log_warn_trigger = 0;
int                  smlevel_0::log_warn_exceed_percent =
                                    SM_LOG_WARN_EXCEED_PERCENT;
ss_m::LOG_WARN_CALLBACK_FUNC
                     smlevel_0::log_warn_callback = 0;
ss_m::LOG_ARCHIVED_CALLBACK_FUNC
                     smlevel_0::log_archived_callback = 0;

smlevel_0::fileoff_t        smlevel_0::chkpt_displacement = 0;

/*
 * _being_xct_mutex: Used to prevent xct creation during volume dismount.
 * Its sole purpose is to be sure that we don't have transactions
 * running while we are  creating or destroying volumes or
 * mounting/dismounting devices, which are generally
 * start-up/shut-down operations for a server.
 */

// Certain operations have to exclude xcts
static srwlock_t          _begin_xct_mutex;

BackupManager* smlevel_0::bk = 0;
vol_m* smlevel_0::vol = 0;
bf_tree_m* smlevel_0::bf = 0;
log_m* smlevel_0::log = 0;
log_core* smlevel_0::clog = 0;
LogArchiver* smlevel_0::logArchiver = 0;
ArchiveMerger* smlevel_0::archiveMerger = 0;

lock_m* smlevel_0::lm = 0;

ErrLog*            smlevel_0::errlog;


char smlevel_0::zero_page[page_sz];

chkpt_m* smlevel_0::chkpt = 0;

restart_m* smlevel_0::recovery = 0;

btree_m* smlevel_0::bt = 0;

ss_m* smlevel_top::SSM = 0;

smlevel_0::xct_impl_t smlevel_0::xct_impl
#ifndef USE_ATOMIC_COMMIT
    = smlevel_0::XCT_TRADITIONAL;
#else
    = smlevel_0::XCT_PLOG;
#endif

/*
 *  Class ss_m code
 */

/*
 *  Order is important!!
 */
int ss_m::_instance_cnt = 0;
sm_options ss_m::_options;

/*
 * NB: reverse function, _make_store_property
 * is defined in dir.cpp -- so far, used only there
 */
ss_m::store_flag_t
ss_m::_make_store_flag(store_property_t property)
{
    store_flag_t flag = st_unallocated;

    switch (property)  {
        case t_regular:
            flag = st_regular;
            break;
        case t_temporary:
            flag = st_tmp;
            break;
        case t_load_file:
            flag = st_load_file;
            break;
        case t_insert_file:
            flag = st_insert_file;
            break;
        case t_bad_storeproperty:
        default:
            W_FATAL_MSG(eINTERNAL, << "bad store property :" << property );
            break;
    }

    return flag;
}


static queue_based_block_lock_t ssm_once_mutex;
ss_m::ss_m(
    const sm_options &options,
    smlevel_0::LOG_WARN_CALLBACK_FUNC callbackwarn /* = NULL */,
    smlevel_0::LOG_ARCHIVED_CALLBACK_FUNC callbackget /* = NULL */,
    bool start /* = true for backward compatibility reason */
)
{
    _options = options;

    sthread_t::initialize_sthreads_package();

    // Save input parameters for future 'startup' calls
    // input parameters cannot be modified after ss_m object has been constructed
    smlevel_0::log_warn_callback  = callbackwarn;
    smlevel_0::log_archived_callback  = callbackget;

    // Start the store during ss_m constructor if caller is asking for it
    if (true == start)
    {
        bool started = startup();
        // If error encountered, raise fatal error if it was not raised already
        if (false == started)
            W_FATAL_MSG(eINTERNAL, << "Failed to start the store from ss_m constructor");
    }
}

bool ss_m::startup()
{
    CRITICAL_SECTION(cs, ssm_once_mutex);
    if (0 == _instance_cnt)
    {
        // Start the store if it is not running currently
        // Caller can start and stop the store independent of construct and destory
        // the ss_m object.

        // Note: before each startup() call, including the initial one from ssm
        //          constructor choicen (default setting currently), caller can
        //          optionally clear the log files and data files if a clean start is
        //          required (no recovery in this case).
        //          If the log files and data files are intact from previous runs,
        //          either normal or crash shutdowns, the startup() call will go
        //          through the recovery logic when starting up the store.
        //          After the store started, caller can call 'format_dev', 'mount_dev',
        //          'generate_new_lvid', 'and create_vol' if caller would like to use
        //          new devics and volumes for operations in the new run.

        _construct_once();
        return true;
    }
    // Store is already running, cannot have multiple instances running concurrently
    return false;
}

bool ss_m::shutdown()
{
    CRITICAL_SECTION(cs, ssm_once_mutex);
    if (0 < _instance_cnt)
    {
        // Stop the store if it is running currently,
        // do not destroy the ss_m object, caller can start the store again using
        // the same ss_m object, therefore all the option setting remain the same

        // Note: If caller would like to use the simulated 'crash' shutdown logic,
        //          caller must call set_shutdown_flag(false) to set the crash
        //          shutdown flag before the shutdown() call.
        //          The simulated crash shutdown flag would be reset in every
        //          startup() call.

        // This is a force shutdown, meaning:
        // Clean shutdown - abort all active in-flight transactions, flush buffer pool
        //                            take a checkpoint which would record the mounted vol
        //                            then destroy all the managers and free memory
        // Dirty shutdown (false == shutdown_clean) - destroy all active in-flight
        //                            transactions without aborting, then destroy all the managers
        //                            and free memory.  No flush and no checkpoint

        _destruct_once();
        return true;
    }
    // If the store is not running currently, no-op
    return true;
}

void
ss_m::_construct_once()
{
    FUNC(ss_m::_construct_once);

    // Use the options and callbacks from ss_m constructor, no change allowed

    // The input paramters were saved during ss_m constructor
    //   smlevel_0::log_warn_callback  = warn;
    //   smlevel_0::log_archived_callback  = get;

    // Clear out the fingerprint map for the smthreads.
    // All smthreads created after this will be compared against
    // this map for duplication.
    smthread_t::init_fingerprint_map();

    if (_instance_cnt++)  {
        // errlog might not be null since in this case there was another instance.
        if(errlog) {
            errlog->clog << fatal_prio
            << "ss_m cannot be instantiated more than once"
             << flushl;
        }
        W_FATAL_MSG(eINTERNAL, << "instantiating sm twice");
    }

    /*
     *  Level 0
     */
    errlog = new ErrLog("ss_m", log_to_unix_file, _options.get_string_option("sm_errlog", "-").c_str());
    if(!errlog) {
        W_FATAL(eOUTOFMEMORY);
    }


    std::string error_loglevel = _options.get_string_option("sm_errlog_level", "error");
    errlog->setloglevel(ErrLog::parse(error_loglevel.c_str()));
    ///////////////////////////////////////////////////////////////
    // Henceforth, all errors can go to ss_m::errlog thus:
    // ss_m::errlog->clog << XXX_prio << ... << flushl;
    // or
    // ss_m::errlog->log(log_XXX, "format...%s..%d..", s, n); NB: no newline
    ///////////////////////////////////////////////////////////////
#if W_DEBUG_LEVEL > 0
	// just to be sure errlog is working
	errlog->clog << debug_prio << "Errlog up and running." << flushl;
#endif

    w_assert1(page_sz >= 1024);

    /*
     *  Reset flags
     */
    shutting_down = false;
    shutdown_clean = true;

    // choose log manager implementation
    std::string logimpl = _options.get_string_option("sm_log_impl", log_core::IMPL_NAME);


    // For Instant Restart testing purpose, determine which
    // internal code path to use
    _set_recovery_mode();

    bf = new bf_tree_m(_options);
    if (! bf) {
        W_FATAL(eOUTOFMEMORY);
    }

    lm = new lock_m(_options);
    if (! lm)  {
        W_FATAL(eOUTOFMEMORY);
    }

    bk = new BackupManager(_options.get_string_option("sm_backup_dir", "."));
    if (! bk) {
        W_FATAL(eOUTOFMEMORY);
    }

    vol = new vol_m(_options);
    if (!vol) {
        W_FATAL(eOUTOFMEMORY);
    }

    /*
     *  Level 1
     */
    smlevel_0::logging_enabled = _options.get_bool_option("sm_logging", true);
    if (logging_enabled)
    {
#ifndef USE_ATOMIC_COMMIT // otherwise, log and clog will point to the same log object
        if (logimpl == logbuf_core::IMPL_NAME) {
            log = new logbuf_core(_options);
        }
        else { // traditional
            log = new log_core(_options);
        }
#else
        /*
         * Centralized log used for atomic commit protocol (by Caetano).
         * See comments in plog.h
         */
        clog = new log_core(_options);
        log = clog;
        w_assert0(log);
#endif

        // LOG ARCHIVER
        bool archiving = _options.get_bool_option("sm_archiving", false);
        if (archiving) {
            logArchiver = new LogArchiver(_options);
            logArchiver->fork();

            bool merging = _options.get_bool_option("sm_async_merging", false);
            if (merging)
            {
                archiveMerger = new ArchiveMerger(_options);
                archiveMerger->fork();
            }
        }
    } else {
        /* Run without logging at your own risk. */
        errlog->clog << warning_prio <<
        "WARNING: Running without logging! Do so at YOUR OWN RISK. "
        << flushl;
    }

    smlevel_0::statistics_enabled = _options.get_bool_option("sm_statistics", true);

    // start buffer pool cleaner when the log module is ready
    W_COERCE(bf->init());

    DBG(<<"Level 2");

    /*
     *  Level 2
     */

    bt = new btree_m;
    if (! bt) {
        W_FATAL(eOUTOFMEMORY);
    }
    bt->construct_once();

    DBG(<<"Level 3");
    /*
     *  Level 3
     */
    chkpt = new chkpt_m;
    if (! chkpt)  {
        W_FATAL(eOUTOFMEMORY);
    }
    // Spawn the checkpoint child thread immediatelly
    chkpt->spawn_chkpt_thread();

    DBG(<<"Level 4");
    /*
     *  Level 4
     */
    SSM = this;

    me()->mark_pin_count();

    _do_restart();

    do_prefetch = _options.get_bool_option("sm_prefetch", false);
    DBG(<<"constructor done");

    // System is opened for user transactions once the function returns
}

void ss_m::_do_restart()
{
    /*
     * Mount the volumes for recovery.  For now, we automatically
     * mount all volumes.  A better solution would be for restart_m
     * to tell us, after analysis, whether any volumes should be
     * mounted.  If not, we can skip the mount/dismount.
     */

    lsn_t     verify_lsn = lsn_t::null;  // verify_lsn is for use_concurrent_commit_restart() only
    lsn_t     redo_lsn = lsn_t::null;    // used if log driven REDO with use_concurrent_XXX_restart()
    lsn_t     last_lsn = lsn_t::null;    // used if page driven REDO with use_concurrent_XXX_restart()
    uint32_t  in_doubt_count = 0;        // used if log driven REDO with use_concurrent_XXX_restart()
    lsn_t     master = log->master_lsn();

    if (_options.get_bool_option("sm_logging", true))
    {
        // Start the recovery process as sequential  operations
        // The recovery manager is on the stack, it will be destroyed once it is
        // out of scope
        restart_m restart;

        // Recovery process, a checkpoint will be taken at the end of recovery
        // Make surethe current operating state is before recovery
        smlevel_0::operating_mode = t_not_started;
        restart.restart(master, verify_lsn, redo_lsn, last_lsn, in_doubt_count);

        // CS TODO: Why did we have to mount and dismount all devices here?
#if 0
        // Perform the low level dismount, remount in higher level
        // and dismount again steps.
        // If running in serial mode, everything is fine.
        // If running in concurrent mode, no final higher level dismount.
        // Special case: the mount operation pre-loads the root page as
        // a side effect, if the root page does not exist on disk (e.g., B-tree
        // has only one page worth of data and was never flushed before crash),
        // the mount operation would detect 'page not exist' condition and zero
        // out the root page, this is bad because if the root page was marked as
        // an in_doubt during Log Analysis, this information would be erased
        // when the page got zero out.  This is handled in bf_tree_m::_preload_root_page
        // to put the in_doubt flag back to the root page.

        // contain the scope of dname[]
        // record all the mounted volumes after recovery.

        std::vector<string> dnames;
        std::vector<vid_t> vids;
        W_COERCE( io->get_vols(0, max_vols, dnames, vids) );

        DBG(<<"Dismount all volumes " << dnames.size());
        // now dismount all of them at the io level, the level where they
        // were mounted during recovery.
        if (true == smlevel_0::use_serial_restart())
            W_COERCE( io->dismount_all(true /*flush*/) );
        else
            W_COERCE( io->dismount_all(true /*flush*/, false /*clear_cb*/) ); // do not clear cb if in concurrent recovery mode
        // now mount all the volumes properly at the sm level.
        // then dismount them and free temp files only if there
        // are no locks held.
        for (int i = 0; i < dnames.size(); i++)
        {
            rc_t rc;
            DBG(<<"Remount volume " << dname[i]);
            rc =  mount_vol(dname[i], vid[i]) ;
            if (rc.is_error())
            {
                ss_m::errlog->clog  << warning_prio
                << "Volume on device " << dname[i]
                << " was only partially formatted; cannot be recovered."
                << flushl;
            }
            else
            {
                // Dismount only if running in serial mode
                if (true == smlevel_0::use_serial_restart()) {
                    // CS: switched to volume dismount after removing device manager
                    /* W_COERCE( _dismount_dev(dname[i])); */
                    W_COERCE(io->dismount(vid[i]));
                }
            }
        }
        delete [] vid;
        for (i = 0; i < max_vols; i++) {
            delete [] dname[i];
        }
        delete [] dname;
#endif
    }

    // Pure on-demand mode must be the same for REDO and UNDO phases
    if (smlevel_0::use_redo_demand_restart() != smlevel_0::use_undo_demand_restart())
    {
        W_FATAL_MSG(fcINTERNAL, << "Inconsistent mode between on-demand REDO and UNDO");
    }

    // Store some information globally in all cases, because although M3 does not use
    // child restart thread initially, it uses child restart thread during normal shutdown
    // REDO from child thread will use redo_lsn, last_lsn and in_doubt_count
    // to control the REDO phase
    smlevel_0::redo_lsn = redo_lsn;              // Log driven REDO, starting point for forward log scan
    smlevel_0::last_lsn = last_lsn;              // page driven REDO, last LSN in Recovery log before system crash
    smlevel_0::in_doubt_count = in_doubt_count;

    if ((false == smlevel_0::use_serial_restart()) &&         // Not serial, so must be concurrent mode
         (false == smlevel_0::use_redo_demand_restart()) &&   // Not pure on-demand redo, so need child thread
         (false == smlevel_0::use_undo_demand_restart()))     // Not pure on-demand undo, so need child thread

    {
        // Log Analysis has completed but no REDO or UNDO yet
        // exception: M5 (ARIES) did the REDO already, but no UNDO
        // Start the recovery process child thread to carry out
        // the REDO and UNDO phases if not in serial or pure on-demand mode
        // which means M2 (traditional), M4 (mixed) and M5 (ARIES) would start the child thread
        // Child thread will carry out both REDO and UNDO
        // for M5 (ARIES), in_doubt count is already 0 therefore no need to REDO again

        if (_options.get_bool_option("sm_logging", true))
        {
            // If we have the recovery process

            // Check the operating mode
            w_assert1(t_in_analysis == smlevel_0::operating_mode);

            // Store commit_lsn globally, concurrent txn uses commit_lsn
            // only if use_concurrent_commit_restart (M2)
            smlevel_0::commit_lsn = verify_lsn;

            if (lsn_t::null != master)
            {
                // If it is not an empty database, then instinate
                // the recovery manager because we need to persist it
                // Note that even if the database is not empty, it does
                // not mean we have recovery work to do in REDO and
                // UNDO phases

                // Commit_lsn could be null if:
                // 1. Empty database
                // 2. No recovery work to do

                w_assert1(!recovery);
                recovery = new restart_m();
                if (! recovery)
                {
                    W_FATAL(eOUTOFMEMORY);
                }
                recovery->spawn_recovery_thread();
            }
        }

        // Continue the process to open system for user transactions immediatelly
        // No buffer pool flush or user checkpoint in this case

        smlevel_0::operating_mode = t_forward_processing;

        // Have the log initialize its reservation accounting.
        // while Recovery REDO and UNDO is happening concurrently
        // the 'activate_reservations' does not affect Recovery task
        // althought the calculation might be off since UNDO might not be
        // completed yet
        if (log)
            log->activate_reservations();

        // Do not flush buffer pool or take checkpoint because recovery
        // is still going on

    }
    else
    {
        // If in serial or pure on-demand mode, change the operating state
        // to allow concurrent transactions to come in
        // No child restart thread for these modes

        smlevel_0::operating_mode = t_forward_processing;

        // Have the log initialize its reservation accounting.
        if(log)
            log->activate_reservations();

        // Force the log after recovery.  The background flush threads exist
        // and might be working due to recovery activities.
        // But to avoid interference with their control structure,
        // we will do this directly.  Take a checkpoint as well.
        if(log)
        {
            bf->force_until_lsn(log->curr_lsn().data());

            // An synchronous checkpoint was taken at the end of recovery
            // This is a asynchronous checkpoint after buffer pool flush
            chkpt->wakeup_and_take();
        }

        // Debug only
        me()->check_pin_count(0);

    }
}

void ss_m::_finish_recovery()
{
    if ((shutdown_clean) && (true == smlevel_0::use_redo_demand_restart()))
    {
        // If we have a clean shutdown and the current system is using
        // pure on-demand recovery (no child restart thread), we might
        // still have a lot of recovery work to do at this point
        // Because we are doing a clean shutdown, we do not want to have
        // leftover restart work, spawn the restart child thread to finish up
        // the restart work first and then shutdown

        w_assert1(!recovery);
        recovery = new restart_m();
        if (recovery)
            recovery->spawn_recovery_thread();
    }

    // get rid of all non-prepared transactions
    // First... disassociate me from any tx
    if(xct()) {
        me()->detach_xct(xct());
    }

    if (recovery)
    {
        // The recovery object is only inistantiated if open system for user
        // transactions during recovery
        w_assert1(false == smlevel_0::use_serial_restart());

        // The destructor of restart_m terminates (no wait if crash shutdown)
        // the child thread if the child thread is still active.
        // The child thread is for Recovery process only, it should terminate
        // itself after the Recovery process completed

        delete recovery;
        recovery = 0;
    }
    // At this point, the restart_m should no longer exist and we are safe to continue the
    // shutdown process
    w_assert1(!recovery);

    // Failure on failure scenarios -
    // Normal shutdown:
    //    Works correctly even if the 2nd shutdown request came in before the first restart
    //    process finished.
    //        Tranditional serial shutdown - System was not opened while restart is going on
    //        Tranditional serial shutdown - The cleanup() call rolls back all in-flight transactions
    //        Pure on-demand shutdown using commit_lsn - The cleanup() call rolls back all
    //                                                                            in-flight transactions
    //        Pure on-demand shutdown using locks - The cleanup() call rolls back all
    //                                                                   in-flight transactions
    //        Mixed mode using locks - The cleanup() call rolls back all in-flight transactions
    //
    // Simulated system crash through 'shutdown_clean flag:
    //    Work correctly with failure on failure scenarions, including on_demand restart
    //    with lock is used and the 2nd failure occurs before the first restart process finished.
    //        Tranditional serial shutdown - System was not opened while restart is going on
    //        Tranditional serial shutdown - The cleanup() call stops all in-flight transactions
    //                                                    without rolling back
    //        Pure on-demand shutdown using commit_lsn - The cleanup() call stops all in-flight
    //                                                    transactions without rolling back
    //        Pure on-demand shutdown using locks - If the 2nd system crash occurs during
    //                                                    Log Analysis, no issue
    //                                                    Otherwise, the cleanup() call stops all in-flight
    //                                                    transactions without rolling them back.
    //                                                    If a user transaction has triggered an on_demand
    //                                                    UNDO and it was in the middle of rolling back the
    //                                                    loser transaction, potentially there might be other
    //                                                    blocked user transactions due to lock conflicts,
    //                                                    and the 2nd system crash occurred, note at this
    //                                                    point no log record generated for all user transactions
    //                                                    bloced on lock conflicts.
    //                                                    During 2nd restart backward log scan Log Analysis
    //                                                    phase, all lock re-acquisions should succeed without
    //                                                    conflicts because the previously blocked user
    //                                                    transactions did not generate log records therefore
    //                                                    no lock re-acquisition and nothing to rollback
    //        Mixed mode using locks - Same as 'pure on-demand shutdown using locks'
    //
    // Genuine system crash:
    //    Similar to simulated system crash, it should work correctly with on_demand restart using lock.
    //        Pure on-demand shutdown using locks - If the 2nd system crash occurs during
    //                                                    Log Analysis, no issue
    //                                                    Otherwise, if system crashed before the entire
    //                                                    on-demand REDO/UNDO finished, and if a user
    //                                                    transaction triggered UNDO was in the middle of
    //                                                    rolling back (which blocked the associated user transaction)
    //                                                    when the system crash occurred, then the lock
    //                                                    re-acquisition process during 2nd restart should not
    //                                                    encounter lock conflict because the previously blocked
    //                                                    user transaction did not generate log record for its
    //                                                    blocked operation, therefore no lock re-acquision and
    //                                                    nothing to rollback
    //        Mixed mode using locks - Same as 'pure on-demand shutdown using locks'
}

void ss_m::_set_recovery_mode()
{
    // For Instant Restart testing purpose
    // which internal restart mode to use?  Default to serial restart (M1) if not specified

    int32_t restart_mode = _options.get_int_option("sm_restart", 1 /*default value*/);
    if (1 == restart_mode)
    {
        // Caller did not specify restart mode, use default (serial mode)
        smlevel_0::restart_internal_mode = (smlevel_0::restart_internal_mode_t)m1_default_restart;
    }
    else
    {
        // Set caller specified restart mode
        smlevel_0::restart_internal_mode = (smlevel_0::restart_internal_mode_t)restart_mode;
    }
}

ss_m::~ss_m()
{
    // This looks like a candidate for pthread_once(), but then smsh
    // would not be able to
    // do multiple startups and shutdowns in one process, alas.
    CRITICAL_SECTION(cs, ssm_once_mutex);

    if (0 < _instance_cnt)
        _destruct_once();
}

void
ss_m::_destruct_once()
{
    FUNC(ss_m::~ss_m);

    --_instance_cnt;

    if (_instance_cnt)  {
        if(errlog) {
            errlog->clog << warning_prio << "ss_m::~ss_m() : \n"
             << "\twarning --- destructor called more than once\n"
             << "\tignored" << flushl;
        } else {
            cerr << "ss_m::~ss_m() : \n"
             << "\twarning --- destructor called more than once\n"
             << "\tignored" << endl;
        }
        return;
    }

    // Set shutting_down so that when we disable bg flushing, if the
    // log flush daemon is running, it won't just try to re-activate it.
    shutting_down = true;


    _finish_recovery();

    // now it's safe to do the clean_up
    // The code for distributed txn (prepared xcts has been deleted, the input paramter
    // in cleanup() is not used
    int nprepared = xct_t::cleanup(false /* don't dispose of prepared xcts */);
    (void) nprepared; // Used only for debugging assert
    if (shutdown_clean) {
        // dismount all volumes which aren't locked by a prepared xct
        // We can't use normal dismounting for the prepared xcts because
        // they would be logged as dismounted. We need to dismount them
        // w/o logging turned on.
        // That happens below.

        W_COERCE( bf->force_all() );
        me()->check_actual_pin_count(0);

        // take a clean checkpoints with the volumes which need
        // to be remounted and the prepared xcts
        // Note that this force_until_lsn will do a direct bpool scan
        // with serial writes since the background flushing has been
        // disabled
        if(log) bf->force_until_lsn(log->curr_lsn());

        // Take a synch checkpoint (blocking) after buffer pool flush but before shutting down
        chkpt->synch_take();

        // from now no more logging and checkpoints will be done
        chkpt->retire_chkpt_thread();
    } else {
        /* still have to close the files, but don't log since not clean !!! */

        // from now no more logging and checkpoints will be done
        chkpt->retire_chkpt_thread();

        log_m* saved_log = log;
        log = 0;                // turn off logging

        log = saved_log;            // turn on logging
    }

    // this should come before xct and log shutdown so that any
    // ongoing restore has a chance to finish cleanly. Should also come after
    // shutdown of buffer, since forcing the buffer requires the volume.
    vol->shutdown(!shutdown_clean);
    delete vol; vol = 0; // io manager

    nprepared = xct_t::cleanup(true /* now dispose of prepared xcts */);
    w_assert1(nprepared == 0);
    w_assert1(xct_t::num_active_xcts() == 0);

    lm->assert_empty(); // no locks should be left

    /*
     *  Level 3
     */
    delete chkpt; chkpt = 0; // NOTE : not level 3 now, but
    // has been retired

    /*
     *  Level 2
     */
    bt->destruct_once();
    delete bt; bt = 0; // btree manager

    /*
     *  Level 1
     */


    // delete the lock manager
    delete lm; lm = 0;

    if (logArchiver) {
        logArchiver->start_shutdown();
        delete logArchiver;
        logArchiver = 0;
    }

    if(log) {
        log->shutdown(); // log joins any subsidiary threads
        // We do not delete the log now; shutdown takes care of that. delete log;
    }
    log = 0;

#ifndef USE_ATOMIC_COMMIT // otherwise clog and log point to the same object
    if(clog) {
        clog->shutdown(); // log joins any subsidiary threads
    }
#endif
    clog = 0;

    W_COERCE(bf->destroy());
    delete bf; bf = 0; // destroy buffer manager last because io/dev are flushing them!
    delete bk; bk = 0;

    /*
     *  Level 0
     */
    if (errlog) {
        delete errlog; errlog = 0;
    }

    /*
     *  free buffer pool memory
     */
     w_rc_t        e;
     char        *unused;
     e = smthread_t::set_bufsize(0, unused);
     if (e.is_error())  {
        cerr << "ss_m: Warning: set_bufsize(0):" << endl << e << endl;
     }
}

void ss_m::set_shutdown_flag(bool clean)
{
    shutdown_clean = clean;
}

// Debugging function
// Returns true if restart is still going on
// Serial restart mode: always return false
// Concurrent restart mode: return true if concurrent restart
//                                          (REDO and UNDO) is active
bool ss_m::in_restart()
{
    // This function can be called only after the system is opened
    // therefore the system is not in recovery when running in serial recovery mode

    if (true == smlevel_0::use_serial_restart())
    {
        return false;
    }
    else if (recovery)
    {
        // The restart object exists and we are not using serial mode
        return recovery->restart_in_progress();
    }
    else
    {
        w_assert1(false == smlevel_0::use_serial_restart());

        // Restart object does not exist, this can happen if the system
        // was started from an empty database and nothing to recover

        return false;
    }
}

// Debugging function
// Returns the status of the specified restart phase
restart_phase_t ss_m::in_log_analysis()
{
    if (true == smlevel_0::use_serial_restart())
    {
        // If in serial mode, by time time caller calls this function
        // we are done with restart already
        return t_restart_phase_done;
    }
    else if (recovery)
    {
        // System is opened after Log Analysis phase
        // If we have the restart object, are we still in Log Analysis?
        if (true == smlevel_0::in_recovery_analysis())
            return t_restart_phase_active;
        else
            return t_restart_phase_done;
    }
    else
    {
        // Restart object does not exist
        return t_restart_phase_done;
    }
}

// Debugging function
// Returns the status of the specified restart phase
restart_phase_t ss_m::in_REDO()
{
    if (true == smlevel_0::use_serial_restart())
    {
        // If in serial mode, by time time caller calls this function
        // we are done with restart already
        return t_restart_phase_done;
    }
    else if (recovery)
    {
        // System is opened after Log Analysis phase
        // If we have the restart object, are we still in REDO?

        // If pure on-demand REDO, we don't know where we are
        if (true == smlevel_0::use_redo_demand_restart())
            return t_restart_phase_unknown;

        // If concurrent REDO (M2 or M4)
        if (true == recovery->redo_in_progress())
            return t_restart_phase_active;  // In REDO
        else
            return t_restart_phase_done;    // Done with REDO
    }
    else
    {
        // Restart object does not exist
        return t_restart_phase_done;
    }
}

// Debugging function
// Returns the status of the specified restart phase
restart_phase_t ss_m::in_UNDO()
{
    if (true == smlevel_0::use_serial_restart())
    {
        // If in serial mode, by time time caller calls this function
        // we are done with restart already
        return t_restart_phase_done;
    }
    else if (recovery)
    {
        // System is opened after Log Analysis phase
        // If we have the restart object, are we still in UNDO?

        // If pure on-demand REDO, we don't know where we are
        if (true == smlevel_0::use_undo_demand_restart())
            return t_restart_phase_unknown;

        // If concurrent REDO (M2 or M4)
        if (true == recovery->undo_in_progress())
            return t_restart_phase_active;       // In UNDO
        else if (true == recovery->redo_in_progress())
            return t_restart_phase_not_active;   // Still in REDO
        else
            return t_restart_phase_done;         // Done with UNDO
    }
    else
    {
        // Restart object does not exist
        return t_restart_phase_done;
    }
}


/*--------------------------------------------------------------*
 *  ss_m::begin_xct()                                *
 *
 *\details
 *
 * You cannot start a transaction while any thread is :
 * - mounting or unmounting a device, or
 * - creating or destroying a volume.
 *--------------------------------------------------------------*/
rc_t
ss_m::begin_xct(
        sm_stats_info_t*             _stats, // allocated by caller
        timeout_in_ms timeout)
{
    SM_PROLOGUE_RC(ss_m::begin_xct, not_in_xct, read_only,  0);
    tid_t tid;
    W_DO(_begin_xct(_stats, tid, timeout));
    return RCOK;
}
rc_t
ss_m::begin_xct(timeout_in_ms timeout)
{
    SM_PROLOGUE_RC(ss_m::begin_xct, not_in_xct, read_only,  0);
    tid_t tid;
    W_DO(_begin_xct(0, tid, timeout));
    return RCOK;
}

/*--------------------------------------------------------------*
 *  ss_m::begin_xct() - for Markos' tests                       *
 *--------------------------------------------------------------*/
rc_t
ss_m::begin_xct(tid_t& tid, timeout_in_ms timeout)
{
    SM_PROLOGUE_RC(ss_m::begin_xct, not_in_xct,  read_only, 0);
    W_DO(_begin_xct(0, tid, timeout));
    return RCOK;
}

rc_t ss_m::begin_sys_xct(bool single_log_sys_xct,
    sm_stats_info_t *stats, timeout_in_ms timeout)
{
    tid_t tid;
    W_DO (_begin_xct(stats, tid, timeout, true, single_log_sys_xct));
    return RCOK;
}


/*--------------------------------------------------------------*
 *  ss_m::commit_xct()                                *
 *--------------------------------------------------------------*/
rc_t
ss_m::commit_xct(sm_stats_info_t*& _stats, bool lazy,
                 lsn_t* plastlsn)
{
    SM_PROLOGUE_RC(ss_m::commit_xct, commitable_xct, read_write, 0);

    W_DO(_commit_xct(_stats, lazy, plastlsn));
    prologue.no_longer_in_xct();

    return RCOK;
}

rc_t
ss_m::commit_sys_xct()
{
    sm_stats_info_t *_stats = NULL;
    W_DO(_commit_xct(_stats, true, NULL)); // always lazy commit
    return RCOK;
}

/*--------------------------------------------------------------*
 *  ss_m::commit_xct_group()                                *
 *--------------------------------------------------------------*/
rc_t
ss_m::commit_xct_group(xct_t *list[], int listlen)
{
    W_DO(_commit_xct_group(list, listlen));
    return RCOK;
}

/*--------------------------------------------------------------*
 *  ss_m::commit_xct()                                          *
 *--------------------------------------------------------------*/
rc_t
ss_m::commit_xct(bool lazy, lsn_t* plastlsn)
{
    SM_PROLOGUE_RC(ss_m::commit_xct, commitable_xct, read_write, 0);

    sm_stats_info_t*             _stats=0;
    W_DO(_commit_xct(_stats,lazy,plastlsn));
    prologue.no_longer_in_xct();
    /*
     * throw away the _stats, since user isn't harvesting...
     */
    delete _stats;

    return RCOK;
}

/*--------------------------------------------------------------*
 *  ss_m::abort_xct()                                *
 *--------------------------------------------------------------*/
rc_t
ss_m::abort_xct(sm_stats_info_t*&             _stats)
{
    SM_PROLOGUE_RC(ss_m::abort_xct, abortable_xct, read_write, 0);

    // Temp removed for debugging purposes only
    // want to see what happens if the abort proceeds (scripts/alloc.10)
    bool was_sys_xct = xct() && xct()->is_sys_xct();
    W_DO(_abort_xct(_stats));
    if (!was_sys_xct) { // system transaction might be nested
        prologue.no_longer_in_xct();
    }

    return RCOK;
}
rc_t
ss_m::abort_xct()
{
    SM_PROLOGUE_RC(ss_m::abort_xct, abortable_xct, read_write, 0);
    sm_stats_info_t*             _stats=0;

    W_DO(_abort_xct(_stats));
    /*
     * throw away _stats, since user is not harvesting them
     */
    delete _stats;
    prologue.no_longer_in_xct();

    return RCOK;
}

/*--------------------------------------------------------------*
 *  ss_m::save_work()                                *
 *--------------------------------------------------------------*/
rc_t
ss_m::save_work(sm_save_point_t& sp)
{
    // For now, consider this a read/write operation since you
    // wouldn't be doing this unless you intended to write and
    // possibly roll back.
    SM_PROLOGUE_RC(ss_m::save_work, in_xct, read_write, 0);
    W_DO( _save_work(sp) );
    return RCOK;
}

/*--------------------------------------------------------------*
 *  ss_m::rollback_work()                            *
 *--------------------------------------------------------------*/
rc_t
ss_m::rollback_work(const sm_save_point_t& sp)
{
    SM_PROLOGUE_RC(ss_m::rollback_work, in_xct, read_write, 0);
    W_DO( _rollback_work(sp) );
    return RCOK;
}

/*--------------------------------------------------------------*
 *  ss_m::num_active_xcts()                            *
 *--------------------------------------------------------------*/
uint32_t
ss_m::num_active_xcts()
{
    return xct_t::num_active_xcts();
}
/*--------------------------------------------------------------*
 *  ss_m::tid_to_xct()                                *
 *--------------------------------------------------------------*/
xct_t* ss_m::tid_to_xct(const tid_t& tid)
{
    return xct_t::look_up(tid);
}

/*--------------------------------------------------------------*
 *  ss_m::xct_to_tid()                                *
 *--------------------------------------------------------------*/
tid_t ss_m::xct_to_tid(const xct_t* x)
{
    w_assert0(x != NULL);
    return x->tid();
}

/*--------------------------------------------------------------*
 *  ss_m::dump_xcts()                                           *
 *--------------------------------------------------------------*/
rc_t ss_m::dump_xcts(ostream& o)
{
    xct_t::dump(o);
    return RCOK;
}

/*--------------------------------------------------------------*
 *  ss_m::state_xct()                                *
 *--------------------------------------------------------------*/
ss_m::xct_state_t ss_m::state_xct(const xct_t* x)
{
    w_assert3(x != NULL);
    return x->state();
}

smlevel_0::fileoff_t ss_m::xct_log_space_needed()
{
    w_assert3(xct() != NULL);
    return xct()->get_log_space_used();
}

rc_t ss_m::xct_reserve_log_space(fileoff_t amt) {
    w_assert3(xct() != NULL);
    return xct()->wait_for_log_space(amt);
}

/*--------------------------------------------------------------*
 *  ss_m::chain_xct()                                *
 *--------------------------------------------------------------*/
rc_t
ss_m::chain_xct( sm_stats_info_t*&  _stats, bool lazy)
{
    SM_PROLOGUE_RC(ss_m::chain_xct, commitable_xct, read_write, 0);
    W_DO( _chain_xct(_stats, lazy) );
    return RCOK;
}
rc_t
ss_m::chain_xct(bool lazy)
{
    SM_PROLOGUE_RC(ss_m::chain_xct, commitable_xct, read_write, 0);
    sm_stats_info_t        *_stats = 0;
    W_DO( _chain_xct(_stats, lazy) );
    /*
     * throw away the _stats, since user isn't harvesting...
     */
    delete _stats;
    return RCOK;
}

rc_t ss_m::flushlog() {
    // forces until the current lsn
    bf->force_until_lsn(log->curr_lsn());
    return (RCOK);
}

/*--------------------------------------------------------------*
 *  ss_m::checkpoint()
 *  For debugging, smsh
 *--------------------------------------------------------------*/
rc_t
ss_m::checkpoint()
{
    // Just kick the chkpt thread
    chkpt->wakeup_and_take();
    return RCOK;
}


/*--------------------------------------------------------------*
 *  ss_m::checkpoint()
 *  For log buffer testing
 *--------------------------------------------------------------*/
rc_t
ss_m::checkpoint_sync()
{
    // Synch chekcpoint!
    chkpt->synch_take();
    return RCOK;
}

rc_t
ss_m::activate_archiver()
{
    if (logArchiver) {
        logArchiver->activate(lsn_t::null, false);
    }
    return RCOK;
}

rc_t
ss_m::activate_merger()
{
    if (archiveMerger) {
        archiveMerger->activate(false);
    }
    return RCOK;
}


rc_t ss_m::force_buffers() {
    return bf->force_all();
}

rc_t ss_m::force_volume(vid_t vol) {
    return bf->force_volume(vol);
}

/*--------------------------------------------------------------*
 *  ss_m::dump_buffers()                            *
 *  For debugging, smsh
 *--------------------------------------------------------------*/
rc_t
ss_m::dump_buffers(ostream &o)
{
    bf->debug_dump(o);
    return RCOK;
}

/*--------------------------------------------------------------*
 *  ss_m::config_info()                                *
 *--------------------------------------------------------------*/
rc_t
ss_m::config_info(sm_config_info_t& info) {
    info.page_size = ss_m::page_sz;

    //however, fixable_page_h.space.acquire aligns() the whole mess (hdr + record)
    //which rounds up the space needed, so.... we have to figure that in
    //here: round up then subtract one aligned entity.
    //
    // OK, now that _data is already aligned, we don't have to
    // lose those 4 bytes.
    info.lg_rec_page_space = btree_page::data_sz;
    info.buffer_pool_size = bf->get_block_cnt() * ss_m::page_sz / 1024;
    info.max_btree_entry_size  = btree_m::max_entry_size();
    info.exts_on_page  = 0;
    info.pages_per_ext = smlevel_0::ext_sz;

    info.logging  = (ss_m::log != 0);

    return RCOK;
}

/*--------------------------------------------------------------*
 *  ss_m::start_log_corruption()                        *
 *--------------------------------------------------------------*/
rc_t
ss_m::start_log_corruption()
{
    SM_PROLOGUE_RC(ss_m::start_log_corruption, in_xct, read_write, 0);
    if(log) {
        // flush current log buffer since all future logs will be
        // corrupted.
        errlog->clog << emerg_prio << "Starting Log Corruption" << flushl;
        log->start_log_corruption();
    }
    return RCOK;
}

/*--------------------------------------------------------------*
 *  ss_m::sync_log()                                *
 *--------------------------------------------------------------*/
rc_t
ss_m::sync_log(bool block)
{
    return log? log->flush_all(block) : RCOK;
}

/*--------------------------------------------------------------*
 *  ss_m::flush_until()                                *
 *--------------------------------------------------------------*/
rc_t
ss_m::flush_until(lsn_t& anlsn, bool block)
{
  return log->flush(anlsn, block);
}

/*--------------------------------------------------------------*
 *  ss_m::get_curr_lsn()                            *
 *--------------------------------------------------------------*/
rc_t
ss_m::get_curr_lsn(lsn_t& anlsn)
{
  anlsn = log->curr_lsn();
  return (RCOK);
}

/*--------------------------------------------------------------*
 *  ss_m::get_durable_lsn()                            *
 *--------------------------------------------------------------*/
rc_t
ss_m::get_durable_lsn(lsn_t& anlsn)
{
  anlsn = log->durable_lsn();
  return (RCOK);
}

void ss_m::dump_page_lsn_chain(std::ostream &o) {
    dump_page_lsn_chain(o, lpid_t::null, lsn_t::max);
}
void ss_m::dump_page_lsn_chain(std::ostream &o, const lpid_t &pid) {
    dump_page_lsn_chain(o, pid, lsn_t::max);
}
void ss_m::dump_page_lsn_chain(std::ostream &o, const lpid_t &pid, const lsn_t &max_lsn) {
    // using static method since restart_m is not guaranteed to be active
    restart_m::dump_page_lsn_chain(o, pid, max_lsn);
}

/*--------------------------------------------------------------*
 *  DEVICE and VOLUME MANAGEMENT                        *
 *--------------------------------------------------------------*/

rc_t
ss_m::mount_vol(const char* path, vid_t& vid)
{
    SM_PROLOGUE_RC(ss_m::mount_vol, not_in_xct, read_only, 0);

    spinlock_write_critical_section cs(&_begin_xct_mutex);

    DBG(<<"mount_vol " << path);

    W_DO(vol->sx_mount(path));
    vid = vol->get(path)->vid();

    return RCOK;
}

rc_t
ss_m::dismount_vol(const char* path)
{
    SM_PROLOGUE_RC(ss_m::mount_vol, not_in_xct, read_only, 0);

    spinlock_write_critical_section cs(&_begin_xct_mutex);

    DBG(<<"dismount_vol " << path);

    W_DO(vol->sx_dismount(path));

    return RCOK;
}

/*--------------------------------------------------------------*
 *  ss_m::get_device_quota()                            *
 *--------------------------------------------------------------*/
rc_t
ss_m::get_device_quota(const char* device, size_t& quota_KB, size_t& quota_used_KB)
{
    SM_PROLOGUE_RC(ss_m::get_device_quota, can_be_in_xct, read_only, 0);

    vol_t* v = vol->get(device);
    size_t page_size_kb = sizeof(generic_page) / 1024;
    quota_used_KB = v->num_used_pages() * page_size_kb;
    quota_KB = v->num_pages() * page_size_kb;
    return RCOK;
}

/*--------------------------------------------------------------*
 *  ss_m::create_vol()                                *
 *--------------------------------------------------------------*/
rc_t
ss_m::create_vol(const char* dev_name, smksize_t quota_KB, vid_t& vid)
{
    SM_PROLOGUE_RC(ss_m::create_vol, not_in_xct, read_only, 0);

    if(quota_KB > sthread_t::max_os_file_size / 1024) {
        return RC(eDEVTOOLARGE);
    }

    spinlock_write_critical_section cs(&_begin_xct_mutex);

    W_DO(vol->sx_format(dev_name,
       shpid_t(quota_KB/(page_sz/1024)),
       vid));

    return RCOK;
}

rc_t ss_m::verify_volume(
    vid_t vid, int hash_bits, verify_volume_result &result)
{
    W_DO(btree_m::verify_volume(vid, hash_bits, result));
    return RCOK;
}

ostream& operator<<(ostream& o, const lpid_t& pid)
{
    return o << "p(" << pid.vol() << '.' << pid.page << ')';
}

istream& operator>>(istream& i, lpid_t& pid)
{
    char c[5];
    memset(c, 0, sizeof(c));
    i >> c[0]        // p
        >> c[1]      // (
        >> pid._vol  // vid
        >> c[2]      // .
        >> pid.page  // shpid
        >> c[3];     // )
    c[4] = '\0';
    if (i)  {
        if (strcmp(c, "p(.)")) {
            i.clear(ios::badbit|i.rdstate());  // error
        }
    }
    return i;
}

#if defined(__GNUC__) && __GNUC_MINOR__ > 6
ostream& operator<<(ostream& o, const smlevel_0::xct_state_t& xct_state)
{
// NOTE: these had better be kept up-to-date wrt the enumeration
// found in sm_base.h
    const char* names[] = {"xct_stale",
                        "xct_active",
                        "xct_prepared",
                        "xct_aborting",
                        "xct_chaining",
                        "xct_committing",
                        "xct_freeing_space",
                        "xct_ended"};

    o << names[xct_state];
    return o;
}
#endif


/*--------------------------------------------------------------*
 *  ss_m::dump_locks()                                *
 *--------------------------------------------------------------*/
rc_t
ss_m::dump_locks(ostream &o)
{
    lm->dump(o);
    return RCOK;
}

rc_t
ss_m::dump_locks() {
  return dump_locks(std::cout);
}



//#ifdef SLI_HOOKS
/*--------------------------------------------------------------*
 *  Enable/Disable Shore-SM features                            *
 *--------------------------------------------------------------*/

void ss_m::set_sli_enabled(bool /* enable */)
{
    fprintf(stdout, "SLI not supported\n");
    //lm->set_sli_enabled(enable);
    //TODO: SHORE-KITS-API
    assert(0);
}

void ss_m::set_elr_enabled(bool /* enable */)
{
    fprintf(stdout, "ELR not supported\n");
    //xct_t::set_elr_enabled(enable);
    //TODO: SHORE-KITS-API
    assert(0);
}

rc_t ss_m::set_log_features(char const* /* features */)
{
    fprintf(stdout, "Aether not integrated\n");
    return (RCOK);
    //return log->set_log_features(features);
    //TODO: SHORE-KITS-API
    assert(0);
}

char const* ss_m::get_log_features()
{
    fprintf(stdout, "Aether not integrated\n");
    return ("NOT-IMPL");
    //return log->get_log_features();
    //TODO: SHORE-KITS-API
    assert(0);
}
//#endif

lil_global_table* ss_m::get_lil_global_table() {
    if (lm) {
        return lm->get_lil_global_table();
    } else {
        return NULL;
    }
}

rc_t ss_m::lock(const lockid_t& n, const okvl_mode& m,
           bool check_only, timeout_in_ms timeout)
{
    W_DO( lm->lock(n, m, false, check_only, timeout) );
    return RCOK;
}


/*--------------------------------------------------------------*
 *  ss_m::unlock()                                *
 *--------------------------------------------------------------*/
/*rc_t
ss_m::unlock(const lockid_t& n)
{
    SM_PROLOGUE_RC(ss_m::unlock, in_xct, read_only, 0);
    W_DO( lm->unlock(n) );
    return RCOK;
}
*/

/*
rc_t
ss_m::query_lock(const lockid_t& n, lock_mode_t& m)
{
    SM_PROLOGUE_RC(ss_m::query_lock, in_xct, read_only, 0);
    W_DO( lm->query(n, m, xct()->tid()) );

    return RCOK;
}
*/

/*****************************************************************
 * Internal/physical-ID version of all the storage operations
 *****************************************************************/

/*--------------------------------------------------------------*
 *  ss_m::_begin_xct(sm_stats_info_t *_stats, timeout_in_ms timeout) *
 *
 * @param[in] _stats  If called by begin_xct without a _stats, then _stats is NULL here.
 *                    If not null, the transaction is instrumented.
 *                    The stats structure may be returned to the
 *                    client through the appropriate version of
 *                    commit_xct, abort_xct, prepare_xct, or chain_xct.
 *--------------------------------------------------------------*/
rc_t
ss_m::_begin_xct(sm_stats_info_t *_stats, tid_t& tid, timeout_in_ms timeout, bool sys_xct,
    bool single_log_sys_xct)
{
    w_assert1(!single_log_sys_xct || sys_xct); // SSX is always system-transaction

    // system transaction can be a nested transaction, so
    // xct() could be non-NULL
    if (!sys_xct && xct() != NULL) {
        return RC (eINTRANS);
    }

    xct_t* x;
    if (sys_xct) {
        x = xct();
        if (single_log_sys_xct && x) {
            // in this case, we don't need an independent transaction object.
            // we just piggy back on the outer transaction
            if (x->is_piggy_backed_single_log_sys_xct()) {
                // SSX can't nest SSX, but we can chain consecutive SSXs.
                ++(x->ssx_chain_len());
            } else {
                x->set_piggy_backed_single_log_sys_xct(true);
            }
            tid = x->tid();
            return RCOK;
        }
        // system transaction doesn't need synchronization with create_vol etc
        // TODO might need to reconsider. but really needs this change now
        x = _new_xct(_stats, timeout, sys_xct, single_log_sys_xct);
    } else {
        spinlock_read_critical_section cs(&_begin_xct_mutex);
        x = _new_xct(_stats, timeout, sys_xct);
        if(log) {
            // This transaction will make no events related to LSN
            // smaller than this. Used to control garbage collection, etc.
            log->get_oldest_lsn_tracker()->enter(reinterpret_cast<uintptr_t>(x), log->curr_lsn());
        }
    }

    if (!x)
        return RC(eOUTOFMEMORY);

    w_assert3(xct() == x);
    w_assert3(x->state() == xct_t::xct_active);
    tid = x->tid();

    return RCOK;
}

xct_t* ss_m::_new_xct(
        sm_stats_info_t* stats,
        timeout_in_ms timeout,
        bool sys_xct,
        bool single_log_sys_xct)
{
    switch (xct_impl) {
    case XCT_PLOG:
        return new plog_xct_t(stats, timeout, sys_xct, single_log_sys_xct);
    default:
        return new xct_t(stats, timeout, sys_xct, single_log_sys_xct, false);
    }
}

/*--------------------------------------------------------------*
 *  ss_m::_commit_xct()                                *
 *--------------------------------------------------------------*/
rc_t
ss_m::_commit_xct(sm_stats_info_t*& _stats, bool lazy,
                  lsn_t* plastlsn)
{
    w_assert3(xct() != 0);
    xct_t* xp = xct();
    xct_t& x = *xp;
    DBG(<<"commit " << ((char *)lazy?" LAZY":"") << x );

    if (x.is_piggy_backed_single_log_sys_xct()) {
        // then, commit() does nothing
        // It just "resolves" the SSX on piggyback
        if (x.ssx_chain_len() > 0) {
            --x.ssx_chain_len(); // multiple SSXs on piggyback
        } else {
            x.set_piggy_backed_single_log_sys_xct(false);
        }
        return RCOK;
    }

    w_assert3(x.state()==xct_active);
    w_assert1(x.ssx_chain_len() == 0);

    W_DO( x.commit(lazy,plastlsn) );

    if(x.is_instrumented()) {
        _stats = x.steal_stats();
        _stats->compute();
    }
    bool was_sys_xct W_IFDEBUG3(= x.is_sys_xct());
    delete xp;
    w_assert3(was_sys_xct || xct() == 0);

    return RCOK;
}

/*--------------------------------------------------------------*
 *  ss_m::_commit_xct_group( xct_t *list[], len)                *
 *--------------------------------------------------------------*/

rc_t
ss_m::_commit_xct_group(xct_t *list[], int listlen)
{
    // We don't care what, if any, xct is attached
    xct_t* x = xct();
    if(x) me()->detach_xct(x);

    DBG(<<"commit group " );

    // 1) verify either all are participating in 2pc
    // in same way (not, prepared, not prepared)
    // Some may be read-only
    // 2) do the first part of the commit for each one.
    // 3) write the group-commit log record.
    // (TODO: we should remove the read-only xcts from this list)
    //
    int participating=0;
    for(int i=0; i < listlen; i++) {
        // verify list
        x = list[i];
        w_assert3(x->state()==xct_active);
    }
    if(participating > 0 && participating < listlen) {
        // some transaction is not participating in external 2-phase commit
        // but others are. Don't delete any xcts.
        // Leave it up to the server to decide how to deal with this; it's
        // a server error.
        return RC(eNOTEXTERN2PC);
    }

    for(int i=0; i < listlen; i++) {
        x = list[i];
        /*
         * Do a partial commit -- all but logging the
         * commit and freeing the locks.
         */
        me()->attach_xct(x);
        {
        SM_PROLOGUE_RC(ss_m::mount_dev, commitable_xct, read_write, 0);
        W_DO( x->commit_as_group_member() );
        }
        w_assert1(me()->xct() == NULL);

        if(x->is_instrumented()) {
            // remove the stats, delete them
            sm_stats_info_t* _stats = x->steal_stats();
            delete _stats;
        }
    }

    // Write group commit record
    // Failure here requires that the server abort them individually.
    // I don't know why the compiler won't convert from a
    // non-const to a const xct_t * list.
    W_DO(xct_t::group_commit((const xct_t **)list, listlen));

    // Destroy the xcts
    for(int i=0; i < listlen; i++) {
        /*
         *  Free all locks for each transaction
         */
        x = list[i];
        w_assert1(me()->xct() == NULL);
        me()->attach_xct(x);
        W_DO(x->commit_free_locks());
        me()->detach_xct(x);
        delete x;
    }
    return RCOK;
}

/*--------------------------------------------------------------*
 *  ss_m::_chain_xct()                                *
 *--------------------------------------------------------------*/
rc_t
ss_m::_chain_xct(
        sm_stats_info_t*&  _stats, /* pass in a new one, get back the old */
        bool lazy)
{
    sm_stats_info_t*  new_stats = _stats;
    w_assert3(xct() != 0);
    xct_t* x = xct();

    W_DO( x->chain(lazy) );
    w_assert3(xct() == x);
    if(x->is_instrumented()) {
        _stats = x->steal_stats();
        _stats->compute();
    }
    x->give_stats(new_stats);

    return RCOK;
}

/*--------------------------------------------------------------*
 *  ss_m::_abort_xct()                                *
 *--------------------------------------------------------------*/
rc_t
ss_m::_abort_xct(sm_stats_info_t*&             _stats)
{
    w_assert3(xct() != 0);
    xct_t* xp = xct();
    xct_t& x = *xp;

    // if this is "piggy-backed" ssx, just end the status
    if (x.is_piggy_backed_single_log_sys_xct()) {
        x.set_piggy_backed_single_log_sys_xct(false);
        return RCOK;
    }

    bool was_sys_xct W_IFDEBUG3(= x.is_sys_xct());

    W_DO( x.abort(true /* save _stats structure */) );
    if(x.is_instrumented()) {
        _stats = x.steal_stats();
        _stats->compute();
    }

    delete xp;
    w_assert3(was_sys_xct || xct() == 0);

    return RCOK;
}

/*--------------------------------------------------------------*
 *  ss_m::save_work()                                *
 *--------------------------------------------------------------*/
rc_t
ss_m::_save_work(sm_save_point_t& sp)
{
    w_assert3(xct() != 0);
    xct_t* x = xct();

    W_DO(x->save_point(sp));
    sp._tid = x->tid();
#if W_DEBUG_LEVEL > 4
    {
        w_ostrstream s;
        s << "save_point @ " << (void *)(&sp)
            << " " << sp
            << " created for tid " << x->tid();
        fprintf(stderr,  "%s\n", s.c_str());
    }
#endif
    return RCOK;
}

/*--------------------------------------------------------------*
 *  ss_m::rollback_work()                            *
 *--------------------------------------------------------------*/
rc_t
ss_m::_rollback_work(const sm_save_point_t& sp)
{
    w_assert3(xct() != 0);
    xct_t* x = xct();
#if W_DEBUG_LEVEL > 4
    {
        w_ostrstream s;
        s << "rollback_work for " << (void *)(&sp)
            << " " << sp
            << " in tid " << x->tid();
        fprintf(stderr,  "%s\n", s.c_str());
    }
#endif
    if (sp._tid != x->tid())  {
        return RC(eBADSAVEPOINT);
    }
    W_DO( x->rollback(sp) );
    return RCOK;
}

/*--------------------------------------------------------------*
 *  ss_m::get_du_statistics()        DU DF
 *--------------------------------------------------------------*/
rc_t
ss_m::get_du_statistics(vid_t vid, sm_du_stats_t& du, bool audit)
{
    SM_PROLOGUE_RC(ss_m::get_du_statistics, in_xct, read_only, 0);
    W_DO(_get_du_statistics(vid, du, audit));
    return RCOK;
}


/*--------------------------------------------------------------*
 *  ss_m::get_du_statistics()        DU DF                    *
 *--------------------------------------------------------------*/
rc_t
ss_m::get_du_statistics(const stid_t& stid, sm_du_stats_t& du, bool audit)
{
    SM_PROLOGUE_RC(ss_m::get_du_statistics, in_xct, read_only, 0);
    W_DO(_get_du_statistics(stid, du, audit));
    return RCOK;
}

/*--------------------------------------------------------------*
 *  ss_m::_get_du_statistics()        DU DF                    *
 *--------------------------------------------------------------*/
rc_t
ss_m::_get_du_statistics( const stid_t& stpgid, sm_du_stats_t& du, bool audit)
{
    // TODO this should take S lock, not IS
    lpid_t root_pid;
    W_DO(open_store(stpgid, root_pid));

    btree_stats_t btree_stats;
    W_DO( bt->get_du_statistics(root_pid, btree_stats, audit));
    if (audit) {
        W_DO(btree_stats.audit());
    }
    du.btree.add(btree_stats);
    du.btree_cnt++;
    return RCOK;
}


/*--------------------------------------------------------------*
 *  ss_m::_get_du_statistics()  DU DF                           *
 *--------------------------------------------------------------*/
rc_t
ss_m::_get_du_statistics(vid_t vid, sm_du_stats_t& du, bool audit)
{
    /*
     * Cannot call this during recovery, even for
     * debugging purposes
     */
    if(smlevel_0::in_recovery()) {
        return RCOK;
    }
    W_DO(lm->intent_vol_lock(vid, audit ? okvl_mode::S : okvl_mode::IS));
    sm_du_stats_t new_stats;

    /*********************************************************
     * First get stats on all the special stores in the volume.
     *********************************************************/

    stid_t stid;

    /**************************************************
     * Now get stats on every other store on the volume
     **************************************************/

    rc_t rc;
    // get du stats on every store
    for (stid_t s(vid, 0); s.store < stnode_page_h::max; s.store++) {
        DBG(<<"look at store " << s);

        store_flag_t flags;
        rc = vol->get(vid)->get_store_flags(s.store, flags);
        if (rc.is_error()) {
            if (rc.err_num() == eBADSTID) {
                DBG(<<"skipping bad STID " << s );
                continue;  // skip any stores that don't exist
            } else {
                return rc;
            }
        }
        DBG(<<" getting stats for store " << s << " flags=" << flags);
        rc = _get_du_statistics(s, new_stats, audit);
        if (rc.is_error()) {
            if (rc.err_num() == eBADSTID) {
                DBG(<<"skipping large object or missing store " << s );
                continue;  // skip any stores that don't show
                           // up in the directory index
                           // in this case it this means stores for
                           // large object pages
            } else {
                return rc;
            }
        }
        DBG(<<"end for loop with s=" << s );
    }

    if (audit) {
        W_DO(new_stats.audit());
    }
    du.add(new_stats);

    return RCOK;
}



/*--------------------------------------------------------------*
 *  ss_m::{enable,disable,set}_fake_disk_latency()              *
 *--------------------------------------------------------------*/
rc_t
ss_m::enable_fake_disk_latency(vid_t vid)
{
    SM_PROLOGUE_RC(ss_m::enable_fake_disk_latency, not_in_xct, read_only, 0);
    vol_t* v = vol->get(vid);
    if (!v) return RC(eBADVOL);
    v->enable_fake_disk_latency();
    return RCOK;
}

rc_t
ss_m::disable_fake_disk_latency(vid_t vid)
{
    SM_PROLOGUE_RC(ss_m::disable_fake_disk_latency, not_in_xct, read_only, 0);
    vol_t* v = vol->get(vid);
    if (!v) return RC(eBADVOL);
    v->disable_fake_disk_latency();
    return RCOK;
}

rc_t
ss_m::set_fake_disk_latency(vid_t vid, const int adelay)
{
    SM_PROLOGUE_RC(ss_m::set_fake_disk_latency, not_in_xct, read_only, 0);
    vol_t* v = vol->get(vid);
    if (!v) return RC(eBADVOL);
    v->set_fake_disk_latency(adelay);
    return RCOK;
}

/*--------------------------------------------------------------*
 *  ss_m::gather_xct_stats()                            *
 *  Add the stats from this thread into the per-xct stats structure
 *  and return a copy in the given struct _stats.
 *  If reset==true,  clear the per-xct copy.
 *  Doing this has the side-effect of clearing the per-thread copy.
 *--------------------------------------------------------------*/
rc_t
ss_m::gather_xct_stats(sm_stats_info_t& _stats, bool reset)
{
    // Use commitable_xct to ensure exactly 1 thread attached for
    // clean collection of all stats,
    // even those that read-only threads would increment.
    //
    SM_PROLOGUE_RC(ss_m::gather_xct_stats, commitable_xct, read_only, 0);

    w_assert3(xct() != 0);
    xct_t& x = *xct();

    if(x.is_instrumented()) {
        DBGTHRD(<<"instrumented, reset= " << reset );
        // detach_xct adds the per-thread stats to the xct's stats,
        // then clears the per-thread stats so that
        // the next time some stats from this thread are gathered like this
        // into an xct, they aren't duplicated.
        // They are added to the global_stats before they are cleared, so
        // they don't get lost entirely.
        me()->detach_xct(&x);
        me()->attach_xct(&x);

        // Copy out the stats structure stored for this xct.
        _stats = x.const_stats_ref();

        if(reset) {
            DBGTHRD(<<"clearing stats " );
            // clear
            // NOTE!!!!!!!!!!!!!!!!!  NOT THREAD-SAFE:
            x.clear_stats();
        }
#ifdef COMMENT
        /* help debugging sort stuff -see also code in bf.cpp  */
        {
            // print -grot
            extern int bffix_SH[];
            extern int bffix_EX[];
        FIXME: THIS CODE IS ROTTEN AND OUT OF DATE WITH tag_t!!!
            static const char *names[] = {
                "t_bad_p",
                "t_alloc_p",
                "t_stnode_p",
                "t_btree_p",
                "none"
                };
            cout << "PAGE FIXES " <<endl;
            for (int i=0; i<=14; i++) {
                    cout  << names[i] << "="
                        << '\t' << bffix_SH[i] << "+"
                    << '\t' << bffix_EX[i] << "="
                    << '\t' << bffix_EX[i] + bffix_SH[i]
                     << endl;

            }
            int sumSH=0, sumEX=0;
            for (int i=0; i<=14; i++) {
                    sumSH += bffix_SH[i];
                    sumEX += bffix_EX[i];
            }
            cout  << "TOTALS" << "="
                        << '\t' << sumSH<< "+"
                    << '\t' << sumEX << "="
                    << '\t' << sumSH+sumEX
                     << endl;
        }
        if(reset) {
            extern int bffix_SH[];
            extern int bffix_EX[];
            for (int i=0; i<=14; i++) {
                bffix_SH[i] = 0;
                bffix_EX[i] = 0;
            }
        }
#endif /* COMMENT */
    } else {
        DBGTHRD(<<"xct not instrumented");
    }

    return RCOK;
}

/*--------------------------------------------------------------*
 *  ss_m::gather_stats()                            *
 *  NOTE: the client is assumed to pass in a copy that's not
 *  referenced by any other threads right now.
 *  Resetting is not an option. Clients have to gather twice, then
 *  subtract.
 *  NOTE: you do not have to be in a transaction to call this.
 *--------------------------------------------------------------*/
rc_t
ss_m::gather_stats(sm_stats_info_t& _stats)
{
    class GatherSmthreadStats : public SmthreadFunc
    {
    public:
        GatherSmthreadStats(sm_stats_info_t &s) : _stats(s)
        {
            new (&_stats) sm_stats_info_t; // clear the stats
            // by invoking the constructor.
        };
        void operator()(const smthread_t& t)
        {
            t.add_from_TL_stats(_stats);
        }
        void compute() { _stats.compute(); }
    private:
        sm_stats_info_t &_stats;
    } F(_stats);

    //Gather all the threads' statistics into the copy given by
    //the client.
    smthread_t::for_each_smthread(F);
    // F.compute();

    // Now add in the global stats.
    // Global stats contain all the per-thread stats that were collected
    // before a per-thread stats structure was cleared.
    // (This happens when per-xct stats get gathered for instrumented xcts.)
    add_from_global_stats(_stats); // from finished threads and cleared stats
	_stats.compute();
    return RCOK;
}

#if W_DEBUG_LEVEL > 0
extern void dump_all_sm_stats();
void dump_all_sm_stats()
{
    static sm_stats_info_t s;
    W_COERCE(ss_m::gather_stats(s));
    w_ostrstream o;
    o << s << endl;
    fprintf(stderr, "%s\n", o.c_str());
}
#endif

ostream &
operator<<(ostream &o, const sm_stats_info_t &s)
{
    o << s.bfht;
    o << s.sm;
    return o;
}


/*--------------------------------------------------------------*
 *  ss_m::get_store_info()                            *
 *--------------------------------------------------------------*/
rc_t
ss_m::get_store_info(
    const stid_t&           stpgid,
    sm_store_info_t&        info)
{
    SM_PROLOGUE_RC(ss_m::get_store_info, in_xct, read_only, 0);
    W_DO(_get_store_info(stpgid, info));
    return RCOK;
}


ostream&
operator<<(ostream& o, smlevel_0::sm_store_property_t p)
{
    if (p == smlevel_0::t_regular)                o << "regular";
    if (p == smlevel_0::t_temporary)                o << "temporary";
    if (p == smlevel_0::t_load_file)                o << "load_file";
    if (p == smlevel_0::t_insert_file)                o << "insert_file";
    if (p == smlevel_0::t_bad_storeproperty)        o << "bad_storeproperty";
    if (p & !(smlevel_0::t_regular
                | smlevel_0::t_temporary
                | smlevel_0::t_load_file
                | smlevel_0::t_insert_file
                | smlevel_0::t_bad_storeproperty))  {
        o << "unknown_property";
        w_assert3(1);
    }
    return o;
}

ostream&
operator<<(ostream& o, smlevel_0::store_flag_t flag) {
    if (flag == smlevel_0::st_unallocated)  o << "|unallocated";
    if (flag & smlevel_0::st_regular)       o << "|regular";
    if (flag & smlevel_0::st_tmp)           o << "|tmp";
    if (flag & smlevel_0::st_load_file)     o << "|load_file";
    if (flag & smlevel_0::st_insert_file)   o << "|insert_file";
    if (flag & smlevel_0::st_empty)         o << "|empty";
    if (flag & !(smlevel_0::st_unallocated
                | smlevel_0::st_regular
                | smlevel_0::st_tmp
                | smlevel_0::st_load_file
                | smlevel_0::st_insert_file
                | smlevel_0::st_empty))  {
        o << "|unknown";
        w_assert3(1);
    }

    return o << "|";
}

ostream&
operator<<(ostream& o, const smlevel_0::store_operation_t op)
{
    const char *names[] = {"delete_store",
                        "create_store",
                        "set_deleting",
                        "set_store_flags",
                        "set_root"};

    if (op <= smlevel_0::t_set_root)  {
        return o << names[op];
    }
    // else:
    w_assert3(1);
    return o << "unknown";
}

ostream&
operator<<(ostream& o, const smlevel_0::store_deleting_t value)
{
    const char *names[] = { "not_deleting_store",
                        "deleting_store",
                        "store_freeing_exts",
                        "unknown_deleting"};

    if (value <= smlevel_0::t_unknown_deleting)  {
        return o << names[value];
    }
    // else:
    w_assert3(1);
    return o << "unknown_deleting_store_value";
}

rc_t
ss_m::log_file_was_archived(const char * logfile)
{
    if(log) return log->file_was_archived(logfile);
    // should be a programming error to get here!
    return RCOK;
}


extern "C" {
/* Debugger-callable functions to dump various SM tables. */

    void        sm_dumplocks()
    {
        if (smlevel_0::lm) {
                W_IGNORE(ss_m::dump_locks(cout));
        }
        else
                cout << "no smlevel_0::lm" << endl;
        cout << flush;
    }

    void   sm_dumpxcts()
    {
        W_IGNORE(ss_m::dump_xcts(cout));
        cout << flush;
    }

    void        sm_dumpbuffers()
    {
        W_IGNORE(ss_m::dump_buffers(cout));
        cout << flush;
    }
}

/*
 * descend to io_m to check the disk containing the given volume
 */
w_rc_t ss_m::dump_vol_store_info(const vid_t &vid)
{
    SM_PROLOGUE_RC(ss_m::dump_vol_store_info, in_xct, read_only,  0);
    return vol->get(vid)->check_disk();
}


w_rc_t
ss_m::log_message(const char * const msg)
{
    SM_PROLOGUE_RC(ss_m::log_message, in_xct, read_write,  0);
    w_ostrstream out;
    out <<  msg << ends;
    return log_comment(out.c_str());
}

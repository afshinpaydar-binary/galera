/*
 * Copyright (C) 2010-2013 Codership Oy <info@codership.com>
 *
 * Using broadcasts instead of signals below to wake flush callers due to
 * theoretical possibility of more than 2 threads involved.
 */

#include "galera_service_thd.hpp"

const uint32_t galera::ServiceThd::A_NONE = 0;

static const uint32_t A_LAST_COMMITTED = 1U <<  0;
static const uint32_t A_RELEASE_SEQNO  = 1U <<  1;
static const uint32_t A_FLUSH          = 1U << 30;
static const uint32_t A_EXIT           = 1U << 31;

void*
galera::ServiceThd::thd_func (void* arg)
{
    galera::ServiceThd* st = reinterpret_cast<galera::ServiceThd*>(arg);
    bool exit = false;

    while (!exit)
    {
        galera::ServiceThd::Data data;

        {
            gu::Lock lock(st->mtx_);

            if (A_NONE == st->data_.act_) lock.wait(st->cond_);

            data = st->data_;
            st->data_.act_ = A_NONE; // clear pending actions

            if (data.act_ & A_FLUSH)
            {
                if (A_FLUSH == data.act_)
                { // no other actions scheduled (all previous are "flushed")
                    log_info << "Service thread queue flushed.";
                    st->flush_.broadcast();
                }
                else
                { // restore flush flag for the next iteration
                    st->data_.act_ |= A_FLUSH;
                }
            }
        }

        exit = ((data.act_ & A_EXIT));

        if (!exit)
        {
            if (data.act_ & A_LAST_COMMITTED)
            {
                static const size_t max_set_attempts(4);
                size_t attempts = 0;
                ssize_t ret;

                do
                {
                    ret = st->gcs_.set_last_applied(data.last_committed_);

                    if (gu_likely(ret != -EINTR && ret != -ETIMEDOUT))
                    {
                        break;
                    }

                    attempts++;

                    // gcs_set_last_applied() may return EINTR if the send
                    // monitor was interruped, this is not a severe error
                    // and in this case there is no need to display a warning
                    // message. Also, you should not warn about the first
                    // failures due to timeout (if the retry counter has
                    // not been exhausted):
                    log_debug << "Attempt " << attempts << ": "
                              << "Failed to report last committed "
                              << data.last_committed_
                              << " (" << strerror (-ret) << ')';
                }
                while (attempts != max_set_attempts);

                if (gu_likely(ret >= 0))
                {
                    log_debug << "Reported last committed: "
                              << data.last_committed_;
                }
                else
                {
                    log_warn << "Failed to report last committed "
                             << data.last_committed_ << ", " << ret
                             << " (" << strerror (-ret) << ')';
                    // @todo: figure out what to do in this case
                }
            }

            if (data.act_ & A_RELEASE_SEQNO)
            {
                try
                {
                    st->gcache_.seqno_release(data.release_seqno_);
                }
                catch (std::exception& e)
                {
                    log_warn << "Exception releasing seqno "
                             << data.release_seqno_ << ": " << e.what();
                }
            }
        }
    }

    return 0;
}

galera::ServiceThd::ServiceThd (GcsI& gcs, gcache::GCache& gcache) :
    gcache_ (gcache),
    gcs_    (gcs),
    thd_    (),
    mtx_    (),
    cond_   (),
    flush_  (),
    data_   ()
{
    gu_thread_create (&thd_, NULL, thd_func, this);
}

galera::ServiceThd::~ServiceThd ()
{
    {
        gu::Lock lock(mtx_);
        data_.act_ = A_EXIT;
        cond_.signal();
        flush_.broadcast();
    }

    gu_thread_join(thd_, NULL);
}

void
galera::ServiceThd::flush()
{
    gu::Lock lock(mtx_);

    if (!(data_.act_ & A_EXIT))
    {
        if (data_.act_ == A_NONE) cond_.signal();
        data_.act_ |= A_FLUSH;
        do { lock.wait(flush_); } while (data_.act_ & A_FLUSH);
    }
}

void
galera::ServiceThd::reset()
{
    gu::Lock lock(mtx_);
    data_.act_ = A_NONE;
    data_.last_committed_ = 0;
}

void
galera::ServiceThd::report_last_committed(gcs_seqno_t seqno)
{
    gu::Lock lock(mtx_);

    if (data_.last_committed_ < seqno)
    {
        data_.last_committed_ = seqno;

        if (data_.act_ == A_NONE) cond_.signal();

        data_.act_ |= A_LAST_COMMITTED;
    }
}

void
galera::ServiceThd::release_seqno(gcs_seqno_t seqno)
{
    gu::Lock lock(mtx_);

    if (data_.release_seqno_ < seqno)
    {
        data_.release_seqno_ = seqno;

        if (data_.act_ == A_NONE) cond_.signal();

        data_.act_ |= A_RELEASE_SEQNO;
    }
}


#if 0

class CDlConn : public dlcontroller
{
        typedef std::list<tDlJob> tDljQueue;
        friend struct ::acng::tDlJob;
        friend class ::acng::dlcontroller;

        tDljQueue m_new_jobs;
        const IDlConFactory &m_conFactory;

#ifdef HAVE_LINUX_EVENTFD
        int m_wakeventfd = -1;
#define fdWakeRead m_wakeventfd
#define fdWakeWrite m_wakeventfd
#else
        int m_wakepipe[2] =
        { -1, -1 };
#define fdWakeRead m_wakepipe[0]
#define fdWakeWrite m_wakepipe[1]
#endif

        /// blacklist for permanently failing hosts, with error message
        tStrMap m_blacklist;
        tSS m_sendBuf, m_inBuf;

        unsigned ExchangeData(mstring &sErrorMsg, tDlStreamHandle &con,
                        tDljQueue &qActive);

        // Disable pipelining for the next # requests. Actually used as crude workaround for the
        // concept limitation (because of automata over a couple of function) and its
        // impact on download performance.
        // The use case: stupid web servers that redirect all requests do that step-by-step, i.e.
        // they get a bunch of requests but return only the first response and then flush the buffer
        // so we process this response and wish to switch to the new target location (dropping
        // the current connection because we don't keep it somehow to background, this is the only
        // download agent we have). This manner perverts the whole principle and causes permanent
        // disconnects/reconnects. In this case, it's beneficial to disable pipelining and send
        // our requests one-by-one. This is done for a while (i.e. the valueof(m_nDisablePling)/2 )
        // times before the operation mode returns to normal.
        int m_nTempPipelineDisable = 0;

        // the default behavior or using or not using the proxy. Will be set
        // if access proxies shall no longer be used.
        bool m_bProxyTot = false;

        // this is a binary factor, meaning how many reads from buffer are OK when
        // speed limiting is enabled
        unsigned m_nSpeedLimiterRoundUp = (unsigned(1) << 16) - 1;
        unsigned m_nSpeedLimitMaxPerTake = MAX_VAL(unsigned);
        unsigned m_nLastDlCount = 0;

        void wake();
        void drain_event_stream();

        void WorkLoop() override;

public:

        CDlConn(const IDlConFactory &pConFactory);

        ~CDlConn();

        void SignalStop() override;

        // dlcon interface
public:
        bool AddJob(const std::shared_ptr<fileitem> &fi, tHttpUrl src, bool isPT, mstring extraHeaders) override;
        bool AddJob(const std::shared_ptr<fileitem> &fi, tRepoResolvResult repoSrc, bool isPT, mstring extraHeaders) override;
};

#ifdef HAVE_LINUX_EVENTFD
inline void CDlConn::wake()
{
        LOGSTART("CDlConn::wake");
        if (fdWakeWrite == -1)
                return;
        while (true)
        {
                auto r = eventfd_write(fdWakeWrite, 1);
                if (r == 0 || (errno != EINTR && errno != EAGAIN))
                        break;
        }

}

inline void CDlConn::drain_event_stream()
{
        LOGSTARTFUNC
        eventfd_t xtmp;
        for (int i = 0; i < 1000; ++i)
        {
                auto tmp = eventfd_read(fdWakeRead, &xtmp);
                if (tmp == 0)
                        return;
                if (errno != EAGAIN)
                        return;
        }
}

#else
void CDlConn::wake()
{
        LOGSTART("CDlConn::wake");
        POKE(fdWakeWrite);
}
inline void CDlConn::awaken_check()
{
        LOGSTART("CDlConn::awaken_check");
        for (char tmp; ::read(m_wakepipe[0], &tmp, 1) > 0;) ;
}

#endif


bool CDlConn::AddJob(const std::shared_ptr<fileitem> &fi, tHttpUrl src, bool isPT, mstring extraHeaders)
{
        if (m_ctrl_hint < 0 || evabase::in_shutdown)
                return false;
        {
                lockguard g(m_handover_mutex);
                m_new_jobs.emplace_back(this, fi, move(src), isPT, move(extraHeaders));
        }
        m_ctrl_hint++;
        wake();
        return true;
}

bool CDlConn::AddJob(const std::shared_ptr<fileitem> &fi, tRepoResolvResult repoSrc, bool isPT, mstring extraHeaders)
{
        if (m_ctrl_hint < 0 || evabase::in_shutdown)
                return false;
        if (! repoSrc.repodata || repoSrc.repodata->m_backends.empty())
                return false;
        if (repoSrc.sRestPath.empty())
                return false;
        {
                lockguard g(m_handover_mutex);
                m_new_jobs.emplace_back(this, fi, move(repoSrc), isPT, move(extraHeaders));
        }
        m_ctrl_hint++;
        wake();
        return true;
}

CDlConn::CDlConn(const IDlConFactory &pConFactory) :
        m_conFactory(pConFactory)
{
        LOGSTART("CDlConn::dlcon");
#ifdef HAVE_LINUX_EVENTFD
        m_wakeventfd = eventfd(0, EFD_NONBLOCK);
        if (m_wakeventfd == -1)
                m_ctrl_hint = -1;
#else
        if (0 == pipe(m_wakepipe))
        {
                set_nb(m_wakepipe[0]);
                set_nb(m_wakepipe[1]);
        }
        else
        {
                m_wakepipe[0] = m_wakepipe[1] = -1;
                m_ctrl_hint = -1;
        }
#endif
        g_nDlCons++;
}

CDlConn::~CDlConn()
{
        LOGSTART("CDlConn::~dlcon, Destroying dlcon");
#ifdef HAVE_LINUX_EVENTFD
        checkforceclose(m_wakeventfd);
#else
        checkforceclose(m_wakepipe[0]);
        checkforceclose(m_wakepipe[1]);
#endif
        g_nDlCons--;
}

void CDlConn::SignalStop()
{
        LOGSTART("CDlConn::SignalStop");
        // stop all activity as soon as possible
        m_ctrl_hint = -1;
        wake();
}

inline unsigned CDlConn::ExchangeData(mstring &sErrorMsg,
                                                                          tDlStreamHandle &con, tDljQueue &inpipe)
{
        LOGSTARTFUNC;
        LOG("qsize: " << inpipe.size() << ", sendbuf size: " << m_sendBuf.size() << ", inbuf size: " << m_inBuf.size());

        fd_set rfds, wfds;
        int r = 0;
        int fd = con ? con->GetFD() : -1;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);

        if (inpipe.empty())
                m_inBuf.clear(); // better be sure about dirty buffer from previous connection

        // no socket operation needed in this case but just process old buffer contents
        bool bReEntered = !m_inBuf.empty();

        loop_again:

        for (;;)
        {
                FD_SET(fdWakeRead, &rfds);
                int nMaxFd = fdWakeRead;

                if (fd != -1)
                {
                        FD_SET(fd, &rfds);
                        nMaxFd = std::max(fd, nMaxFd);

                        if (!m_sendBuf.empty())
                        {
                                ldbg("Needs to send " << m_sendBuf.size() << " bytes");
                                FD_SET(fd, &wfds);
                        }
#ifdef HAVE_SSL
                        else if (con->GetBIO() && BIO_should_write(con->GetBIO()))
                        {
                                ldbg(
                                                "NOTE: OpenSSL wants to write although send buffer is empty!");
                                FD_SET(fd, &wfds);
                        }
#endif
                }

                ldbg("select dlcon");

                // jump right into data processing but only once
                if (bReEntered)
                {
                        bReEntered = false;
                        goto proc_data;
                }

                r = select(nMaxFd + 1, &rfds, &wfds, nullptr,
                                CTimeVal().ForNetTimeout());
                ldbg("returned: " << r << ", errno: " << tErrnoFmter());
                if (m_ctrl_hint < 0)
                        return HINT_RECONNECT_NOW;

                if (r == -1)
                {
                        if (EINTR == errno)
                                continue;
                        if (EBADF == errno) // that some times happen for no obvious reason
                                return HINT_RECONNECT_NOW;
                        tErrnoFmter fer("FAILURE: select, ");
                        LOG(fer);
                        sErrorMsg = string("Internal malfunction, ") + fer;
                        return HINT_RECONNECT_NOW | EFLAG_JOB_BROKEN | EFLAG_MIRROR_BROKEN;
                }
                else if (r == 0) // looks like a timeout
                {
                        sErrorMsg = "Connection timeout";
                        LOG(sErrorMsg);

                        // was there anything to do at all?
                        if (inpipe.empty())
                                return HINT_SWITCH;

                        return (HINT_RECONNECT_NOW| EFLAG_JOB_BROKEN);
                }

                if (FD_ISSET(fdWakeRead, &rfds))
                {
                        drain_event_stream();
                        return HINT_SWITCH;
                }

                if (fd >= 0)
                {
                        if (FD_ISSET(fd, &wfds))
                        {
                                FD_CLR(fd, &wfds);

#ifdef HAVE_SSL
                                if (con->GetBIO())
                                {
                                        int s = BIO_write(con->GetBIO(), m_sendBuf.rptr(),
                                                        m_sendBuf.size());
                                        ldbg(
                                                        "tried to write to SSL, " << m_sendBuf.size() << " bytes, result: " << s);
                                        if (s > 0)
                                                m_sendBuf.drop(s);
                                }
                                else
                                {
#endif
                                        ldbg("Sending data...\n" << m_sendBuf);
                                        int s = ::send(fd, m_sendBuf.data(), m_sendBuf.length(),
                                        MSG_NOSIGNAL);
                                        ldbg(
                                                        "Sent " << s << " bytes from " << m_sendBuf.length() << " to " << con.get());
                                        if (s < 0)
                                        {
                                                // EAGAIN is weird but let's retry later, otherwise reconnect
                                                if (errno != EAGAIN && errno != EINTR)
                                                {
                                                        sErrorMsg = "Send failed";
                                                        return EFLAG_LOST_CON;
                                                }
                                        }
                                        else
                                                m_sendBuf.drop(s);

                                }
#ifdef HAVE_SSL
                        }
#endif
                }

                if (fd >= 0 && (FD_ISSET(fd, &rfds)
#ifdef HAVE_SSL
                                || (con->GetBIO() && BIO_should_read(con->GetBIO()))
#endif
                                ))
                {
                        if (cfg::maxdlspeed != cfg::RESERVED_DEFVAL)
                        {
                                auto nCntNew = g_nDlCons.load();
                                if (m_nLastDlCount != nCntNew)
                                {
                                        m_nLastDlCount = nCntNew;

                                        // well, split the bandwidth
                                        auto nSpeedNowKib = uint(cfg::maxdlspeed) / nCntNew;
                                        auto nTakesPerSec = nSpeedNowKib / 32;
                                        if (!nTakesPerSec)
                                                nTakesPerSec = 1;
                                        m_nSpeedLimitMaxPerTake = nSpeedNowKib * 1024
                                                        / nTakesPerSec;
                                        auto nIntervalUS = 1000000 / nTakesPerSec;
                                        auto nIntervalUS_copy = nIntervalUS;
                                        // creating a bitmask
                                        for (m_nSpeedLimiterRoundUp = 1, nIntervalUS /= 2;
                                                        nIntervalUS; nIntervalUS >>= 1)
                                                m_nSpeedLimiterRoundUp = (m_nSpeedLimiterRoundUp << 1)
                                                                | 1;
                                        m_nSpeedLimitMaxPerTake = uint(
                                                        double(m_nSpeedLimitMaxPerTake)
                                                                        * double(m_nSpeedLimiterRoundUp)
                                                                        / double(nIntervalUS_copy));
                                }
                                // waiting for the next time slice to get data from buffer
                                timeval tv;
                                if (0 == gettimeofday(&tv, nullptr))
                                {
                                        auto usNext = tv.tv_usec | m_nSpeedLimiterRoundUp;
                                        usleep(usNext - tv.tv_usec);
                                }
                        }
#ifdef HAVE_SSL
                        if (con->GetBIO())
                        {
                                r = BIO_read(con->GetBIO(), m_inBuf.wptr(),
                                                std::min(m_nSpeedLimitMaxPerTake, m_inBuf.freecapa()));
                                if (r > 0)
                                        m_inBuf.got(r);
                                else
                                        // <=0 doesn't mean an error, only a double check can tell
                                        r = BIO_should_read(con->GetBIO()) ? 1 : -errno;
                        }
                        else
#endif
                        {
                                r = m_inBuf.sysread(fd, m_nSpeedLimitMaxPerTake);
                        }

#ifdef DISCO_FAILURE
#warning DISCO_FAILURE active!
                        if((random() & 0xff) < 10)
                        {
                                LOG("\n#################\nFAKING A FAILURE\n###########\n");
                                r=0;
                                errno = EROFS;
                                //r = -errno;
//				shutdown(con.get()->GetFD(), SHUT_RDWR);
                        }
#endif

                        if (r == -EAGAIN || r == -EWOULDBLOCK)
                        {
                                ldbg("why EAGAIN/EINTR after getting it from select?");
//				timespec sleeptime = { 0, 432000000 };
//				nanosleep(&sleeptime, nullptr);
                                goto loop_again;
                        }
                        else if (r == 0)
                        {
                                dbgline;
                                sErrorMsg = "Connection closed, check DlMaxRetries";
                                LOG(sErrorMsg);
                                return EFLAG_LOST_CON;
                        }
                        else if (r < 0) // other error, might reconnect
                        {
                                dbgline;
                                // pickup the error code for later and kill current connection ASAP
                                sErrorMsg = tErrnoFmter();
                                return EFLAG_LOST_CON;
                        }

                        proc_data:

                        if (inpipe.empty())
                        {
                                ldbg("FIXME: unexpected data returned?");
                                sErrorMsg = "Unexpected data";
                                return EFLAG_LOST_CON;
                        }

                        while (!m_inBuf.empty())
                        {

                                //ldbg("Processing job for " << inpipe.front().RemoteUri(false));
                                dbgline;
                                unsigned res = inpipe.front().ProcessIncomming(m_inBuf, false);
                                //ldbg("... incoming data processing result: " << res << ", emsg: " << inpipe.front().sErrorMsg);
                                LOG("res = " << res);

                                if (res & EFLAG_MIRROR_BROKEN)
                                {
                                        ldbg("###### BROKEN MIRROR ####### on " << con.get());
                                }

                                if (HINT_MORE == res)
                                        goto loop_again;

                                if (HINT_DONE & res)
                                {
                                        // just in case that server damaged the last response body
                                        con->KnowLastFile(WEAK_PTR<fileitem>(inpipe.front().m_pStorage));

                                        inpipe.pop_front();
                                        if (HINT_RECONNECT_NOW & res)
                                                return HINT_RECONNECT_NOW; // with cleaned flags

                                        LOG(
                                                        "job finished. Has more? " << inpipe.size() << ", remaining data? " << m_inBuf.size());

                                        if (inpipe.empty())
                                        {
                                                LOG("Need more work");
                                                return HINT_SWITCH;
                                        }

                                        LOG("Extract more responses");
                                        continue;
                                }

                                if (HINT_TGTCHANGE & res)
                                {
                                        /* If the target was modified for internal redirection then there might be
                                         * more responses of that kind in the queue. Apply the redirection handling
                                         * to the rest as well if possible without having side effects.
                                         */
                                        auto it = inpipe.begin();
                                        for (++it; it != inpipe.end(); ++it)
                                        {
                                                unsigned rr = it->ProcessIncomming(m_inBuf, true);
                                                // just the internal rewriting applied and nothing else?
                                                if ( HINT_TGTCHANGE != rr)
                                                {
                                                        // not internal redirection or some failure doing it
                                                        m_nTempPipelineDisable = 30;
                                                        return (HINT_TGTCHANGE | HINT_RECONNECT_NOW);
                                                }
                                        }
                                        // processed all inpipe stuff but if the buffer is still not empty then better disconnect
                                        return HINT_TGTCHANGE | (m_inBuf.empty() ? 0 : HINT_RECONNECT_NOW);
                                }

                                // else case: error handling, pass to main loop
                                if (HINT_KILL_LAST_FILE & res)
                                        con->KillLastFile();
                                setIfNotEmpty(sErrorMsg, inpipe.front().sErrorMsg);
                                return res;
                        }
                        return HINT_DONE; // input buffer consumed
                }
        }

        ASSERT(!"Unreachable");
        sErrorMsg = "Internal failure";
        return EFLAG_JOB_BROKEN | HINT_RECONNECT_NOW;
}

void CDlConn::WorkLoop()
{
        LOGSTART("CDlConn::WorkLoop");
        string sErrorMsg;
        m_inBuf.clear();

        if (!m_inBuf.setsize(cfg::dlbufsize))
        {
                log::err("Out of memory");
                return;
        }

        if (fdWakeRead < 0 || fdWakeWrite < 0)
        {
                USRERR("Error creating pipe file descriptors");
                return;
        }

        tDljQueue next_jobs, active_jobs;

        tDtorEx allJobReleaser([&]()
        {	next_jobs.clear(); active_jobs.clear();});

        tDlStreamHandle con;
        unsigned loopRes = 0;

        bool bStopRequesting = false; // hint to stop adding request headers until the connection is restarted
        bool bExpectRemoteClosing = false;
        int nLostConTolerance = MAX_RETRY;

        auto BlacklistMirror = [&](tDlJob &job)
        {
                LOGSTARTFUNCx(job.GetPeerHost().ToURI(false));
                m_blacklist[job.GetPeerHost().GetHostPortKey()] = sErrorMsg;
        };

        auto prefProxy = [&](tDlJob &cjob) -> const tHttpUrl*
        {
                if(m_bProxyTot)
                return nullptr;

                if(cjob.m_pRepoDesc && cjob.m_pRepoDesc->m_pProxy
                                && !cjob.m_pRepoDesc->m_pProxy->sHost.empty())
                {
                        return cjob.m_pRepoDesc->m_pProxy;
                }
                return cfg::GetProxyInfo();
        };
        int lastCtrlMark = -2;
        while (true) // outer loop: jobs, connection handling
        {
                // init state or transfer loop jumped out, what are the needed actions?
                LOG("New next_jobs: " << next_jobs.size());

                if (m_ctrl_hint < 0 || evabase::in_shutdown) // check for ordered shutdown
                {
                        /* The no-more-users checking logic will purge orphaned items from the inpipe
                         * queue. When the connection is dirty after that, it will be closed in the
                         * ExchangeData() but if not then it can be assumed to be clean and reusable.
                         */
                        if (active_jobs.empty())
                        {
                                if (con)
                                        m_conFactory.RecycleIdleConnection(con);
                                return;
                        }
                }
                int newCtrlMark = m_ctrl_hint;
                if (newCtrlMark != lastCtrlMark)
                {
                        lastCtrlMark = newCtrlMark;
                        lockguard g(m_handover_mutex);
                        next_jobs.splice(next_jobs.begin(), m_new_jobs);
                }
                dbgline;
                if (next_jobs.empty() && active_jobs.empty())
                        goto go_select;
                // parent will notify RSN
                dbgline;
                if (!con)
                {
                        dbgline;
                        // cleanup after the last connection - send buffer, broken next_jobs, ...
                        m_sendBuf.clear();
                        m_inBuf.clear();
                        active_jobs.clear();

                        bStopRequesting = false;

                        for (tDljQueue::iterator it = next_jobs.begin();
                                        it != next_jobs.end();)
                        {
                                if (it->SetupJobConfig(sErrorMsg, m_blacklist))
                                        ++it;
                                else
                                {
                                        setIfNotEmpty2(it->sErrorMsg, sErrorMsg,
                            "Broken mirror or incorrect configuration");
                                        it = next_jobs.erase(it);
                                }
                        }
                        if (next_jobs.empty())
                        {
                                LOG("no next_jobs left, start waiting")
                                goto go_select;
                                // nothing left, might receive new next_jobs soon
                        }

                        bool bUsed = false;
                        ASSERT(!next_jobs.empty());
                        auto doconnect = [&](const tHttpUrl &tgt, int timeout, bool fresh)
                        {
                                bExpectRemoteClosing = false;

                                for(auto& j: active_jobs)
                                        j.ResetStreamState();
                                for(auto& j: next_jobs)
                                        j.ResetStreamState();

                                return m_conFactory.CreateConnected(tgt.sHost,
                                                tgt.GetPort(),
                                                sErrorMsg,
                                                &bUsed,
                                                next_jobs.front().GetConnStateTracker(),
                                                IFSSLORFALSE(tgt.bSSL),
                                                timeout, fresh);
                        };

                        auto &cjob = next_jobs.front();
                        auto proxy = prefProxy(cjob);
                        auto &peerHost = cjob.GetPeerHost();

#ifdef HAVE_SSL
                        if (peerHost.bSSL)
                        {
                                if (proxy)
                                {
                                        con = doconnect(*proxy,
                                                        cfg::optproxytimeout > 0 ?
                                                                        cfg::optproxytimeout : cfg::nettimeout,
                                                        false);
                                        if (con)
                                        {
                                                if (!con->StartTunnel(peerHost, sErrorMsg,
                                                                &proxy->sUserPass, true))
                                                        con.reset();
                                        }
                                }
                                else
                                        con = doconnect(peerHost, cfg::nettimeout, false);
                        }
                        else
#endif
                        {
                                if (proxy)
                                {
                                        con = doconnect(*proxy,
                                                        cfg::optproxytimeout > 0 ?
                                                                        cfg::optproxytimeout : cfg::nettimeout,
                                                        false);
                                }
                                else
                                        con = doconnect(peerHost, cfg::nettimeout, false);
                        }

                        if (!con && proxy && cfg::optproxytimeout > 0)
                        {
                                ldbg("optional proxy broken, disable");
                                m_bProxyTot = true;
                                proxy = nullptr;
                                cfg::MarkProxyFailure();
                                con = doconnect(peerHost, cfg::nettimeout, false);
                        }

                        ldbg("connection valid? " << bool(con) << " was fresh? " << !bUsed);

                        if (con)
                        {
                                ldbg("target? [" << con->GetHostname() << "]:" << con->GetPort());

                                // must test this connection, just be sure no crap is in the pipe
                                if (bUsed && check_read_state(con->GetFD()))
                                {
                                        ldbg("code: MoonWalker");
                                        con.reset();
                                        continue;
                                }
                        }
                        else
                        {
                                BlacklistMirror(cjob);
                                continue; // try the next backend
                        }
                }

                // connection should be stable now, prepare all jobs and/or move to pipeline
                while (!bStopRequesting && !next_jobs.empty()
                                && int(active_jobs.size()) <= cfg::pipelinelen)
                {
                        auto &frontJob = next_jobs.front();

                        if (!frontJob.SetupJobConfig(sErrorMsg, m_blacklist))
                        {
                                // something weird happened to it, drop it and let the client care
                                next_jobs.pop_front();
                                continue;
                        }

                        auto &tgt = frontJob.GetPeerHost();
                        // good case, direct or tunneled connection
                        bool match = (tgt.sHost == con->GetHostname()
                                        && tgt.GetPort() == con->GetPort());
                        const tHttpUrl *proxy = nullptr; // to be set ONLY if PROXY mode is used

                        // if not exact and can be proxied, and is this the right proxy?
                        if (!match)
                        {
                                proxy = prefProxy(frontJob);
                                if (proxy)
                                {
                                        /*
                                         * SSL over proxy uses HTTP tunnels (CONNECT scheme) so the check
                                         * above should have matched before.
                                         */
                                        if (!tgt.bSSL)
                                                match = (proxy->sHost == con->GetHostname()
                                                                && proxy->GetPort() == con->GetPort());
                                }
                                // else... host changed and not going through the same proxy -> fail
                        }

                        if (!match)
                        {
                                LOG(
                                                "host mismatch, new target: " << tgt.sHost << ":" << tgt.GetPort());
                                bStopRequesting = true;
                                break;
                        }

                        frontJob.AppendRequest(m_sendBuf, proxy);
                        LOG("request headers added to buffer");
                        auto itSecond = next_jobs.begin();
                        active_jobs.splice(active_jobs.end(), next_jobs, next_jobs.begin(),
                                        ++itSecond);

                        if (m_nTempPipelineDisable > 0)
                        {
                                bStopRequesting = true;
                                --m_nTempPipelineDisable;
                                break;
                        }
                }

                //ldbg("Request(s) cooked, buffer contents: " << m_sendBuf);
                //ASSERT(!m_sendBuf.empty());
                // FIXME: this is BS in case of SSL (rekeying) but what was the idea?

                go_select:

                // inner loop: plain communication until something happens. Maybe should use epoll here?
                loopRes = ExchangeData(sErrorMsg, con, active_jobs);

                ldbg("loopRes: "<< loopRes
                         << " = " << tSS::BitPrint(" | ",
                                                                           loopRes,
                                                                           BITNNAME(HINT_RECONNECT_NOW)
                                                                           , BITNNAME(HINT_DONE)
                                                                           , BITNNAME(HINT_KILL_LAST_FILE)
                                                                           , BITNNAME(HINT_MORE)
                                                                           , BITNNAME(HINT_SWITCH)
                                                                           , BITNNAME(HINT_TGTCHANGE)
                                                                           , BITNNAME(EFLAG_JOB_BROKEN)
                                                                           , BITNNAME(EFLAG_LOST_CON)
                                                                           , BITNNAME(EFLAG_STORE_COLLISION)
                                                                           , BITNNAME(EFLAG_MIRROR_BROKEN)
                                                                           , BITNNAME(HINT_RECONNECT_SOON)
                ) );

                bExpectRemoteClosing |= (loopRes & HINT_RECONNECT_SOON);

                if (m_ctrl_hint < 0 || evabase::in_shutdown)
                        return;

                /* check whether we have a pipeline stall. This may happen because a) we are done or
                 * b) because of the remote hostname change or c) the client stopped sending tasks.
                 * Anyhow, that's a reason to put the connection back into the shared pool so either we
                 * get it back from the pool in the next workloop cycle or someone else gets it and we
                 * get a new connection for the new host later.
                 * */
                if (active_jobs.empty())
                {
                        // all requests have been processed (client done, or pipeline stall, who cares)
                        dbgline;

                        // no matter what happened, that stop flag is now irrelevant
                        bStopRequesting = false;

                        // no error bits set, not busy -> this connection is still good, recycle properly
                        constexpr auto all_err = HINT_RECONNECT_NOW | HINT_RECONNECT_SOON | EFLAG_JOB_BROKEN | EFLAG_LOST_CON
                                        | EFLAG_MIRROR_BROKEN;
                        if (con && !(loopRes & all_err))
                        {
                                dbgline;
                                m_conFactory.RecycleIdleConnection(con);
                                continue;
                        }
                }

                /*
                 * Here we go if the inpipe is still not processed or there have been errors
                 * needing special handling.
                 */

                if ((HINT_RECONNECT_NOW | EFLAG_LOST_CON) & loopRes)
                {
                        dbgline;
                        con.reset();
                        m_inBuf.clear();
                        m_sendBuf.clear();
                }

                if (loopRes & HINT_TGTCHANGE)
                {
                        // short queue continues next_jobs with rewritten targets, so
                        // reinsert them into the new task list and continue

                        // if conn was not reset above then it should be in good shape
                        m_conFactory.RecycleIdleConnection(con);
                        goto move_jobs_back_to_q;
                }

                if ((EFLAG_LOST_CON & loopRes) && !active_jobs.empty())
                {
                        dbgline;
                        // disconnected by OS... give it a chance, or maybe not...
                        if (! bExpectRemoteClosing)
                        {
                                dbgline;
                                if (--nLostConTolerance <= 0)
                                {
                                        dbgline;
                                        BlacklistMirror(active_jobs.front());
                                        nLostConTolerance = MAX_RETRY;
                                }
                        }
                        con.reset();

                        if (! (HINT_DONE & loopRes))
                        {
                                // trying to resume that job secretly, unless user disabled the use of range (we
                                // cannot resync the sending position ATM, throwing errors to user for now)
                                if (cfg::vrangeops <= 0 && active_jobs.front().m_fiAttr.bVolatile)
                                        loopRes |= EFLAG_JOB_BROKEN;
                                else
                                        active_jobs.front().m_DlState = tDlJob::STATE_GETHEADER;
                        }
                }

                if (loopRes & (HINT_DONE | HINT_MORE))
                {
                        sErrorMsg.clear();
                        continue;
                }

                //
                // regular required post-processing done here, now handle special conditions
                //

                if (HINT_SWITCH == loopRes)
                        continue;

                // resolving the "fatal error" situation, push the pipelined job back to new, etc.

                if ((EFLAG_MIRROR_BROKEN & loopRes) && !active_jobs.empty())
                        BlacklistMirror(active_jobs.front());

                if ((EFLAG_JOB_BROKEN & loopRes) && !active_jobs.empty())
                {
                        setIfNotEmpty(active_jobs.front().sErrorMsg, sErrorMsg);

                        active_jobs.pop_front();

                        if (EFLAG_STORE_COLLISION & loopRes)
                        {
                                // stupid situation, both users downloading the same stuff - and most likely in the same order
                                // if one downloader runs a step ahead (or usually many steps), drop all items
                                // already processed by it and try to continue somewhere else.
                                // This way, the overall number of collisions and reconnects is minimized

                                for (auto pJobList :
                                { &active_jobs, &next_jobs })
                                {
                                        auto &joblist(*pJobList);
                                        for (auto it = joblist.begin(); it != joblist.end();)
                                        {
                                                // someone else is doing it -> drop
                                                if (it->m_pStorage
                                                                && it->m_pStorage->GetStatus()
                                                                                >= fileitem::FIST_DLRECEIVING)
                                                        it = joblist.erase(it);
                                                else
                                                        ++it;
                                        }
                                };
                        }
                }

                move_jobs_back_to_q: next_jobs.splice(next_jobs.begin(), active_jobs);
        }
}

std::shared_ptr<dlcontroller> dlcontroller::CreateRegular(const IDlConFactory &pConFactory)
{
        return make_shared<CDlConn>(pConFactory);
}
#endif

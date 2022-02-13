

#warning extract relevant bits into atransport class
#if 0

/**
 * Helper which implements a custom connection class that runs through a specified Unix Domain
 * Socket (see base class for the name).
 */
struct TUdsFactory : public ::acng::IDlConFactory
{
    void RecycleIdleConnection(tDlStreamHandle &) const override
    {
        // keep going, no recycling/restoring
    }
    tDlStreamHandle CreateConnected(cmstring&, uint16_t, mstring& sErrorOut, bool*,
            tRepoUsageHooks*, bool, int, bool) const override
    {
        struct udsconnection: public tcpconnect
        {
            bool failed = false;
            udsconnection() : tcpconnect(nullptr)
            {
                // some static and dummy parameters, and invalidate SSL for sure
                m_ssl = nullptr;
                m_bio = nullptr;
                m_sHostName = FAKE_UDS_HOSTNAME;
                m_nPort = 0;

                m_conFd = socket(PF_UNIX, SOCK_STREAM, 0);
                if (m_conFd < 0)
                {
                    failed = true;
                    return;
                }
                struct sockaddr_un addr;
                addr.sun_family = PF_UNIX;
                strcpy(addr.sun_path, cfg::udspath.c_str());
                socklen_t adlen = cfg::adminpath.length() + 1 + offsetof(struct sockaddr_un, sun_path);
                if (connect(m_conFd, (struct sockaddr*) &addr, adlen))
                {
                    DBGQLOG(tErrnoFmter("connect result: "));
                    checkforceclose(m_conFd);
                    failed = true;
                    return;
                }
                // basic identification needed
                tSS ids;
                ids << "GET / HTTP/1.0\r\nX-Original-Source: localhost\r\n\r\n";
                if (!ids.send(m_conFd))
                {
                    failed = true;
                    return;
                }
            }
        };
        auto ret = make_shared<udsconnection>();
        // mimic regular processing of a bad result here!
        if(ret && ret->failed) ret.reset();
        if(!ret) sErrorOut = "912 Cannot establish control connection";
        return ret;
    }
};

#endif

#if SUPPWHASH
void ssl_init()
{
#ifdef HAVE_SSL
    SSL_load_error_strings();
    ERR_load_BIO_strings();
    ERR_load_crypto_strings();
    ERR_load_SSL_strings();
    OpenSSL_add_all_algorithms();
    SSL_library_init();
#endif
}
#endif


#if 0
void parse_options(int argc, const char **argv, function<void (LPCSTR)> f)
{
    LPCSTR szCfgDir=CFGDIR;
    std::vector<LPCSTR> validargs, nonoptions;
    bool ignoreCfgErrors = false;

    for (auto p=argv; p<argv+argc; p++)
    {
        if (!strncmp(*p, "-h", 2))
            usage();
        else if (!strcmp(*p, "-c"))
        {
            ++p;
            if (p < argv + argc)
                szCfgDir = *p;
            else
                usage(2);
        }
        else if(!strcmp(*p, "--verbose"))
            g_bVerbose=true;
        else if(!strcmp(*p, "-i"))
            ignoreCfgErrors = true;
        else if(**p) // not empty
            validargs.emplace_back(*p);

#if SUPPWHASH
#warning FIXME
        else if (!strncmp(*p, "-H", 2))
            exit(hashpwd());
#endif
    }

    if(szCfgDir)
    {
        Cstat info(szCfgDir);
        if(!info || !S_ISDIR(info.info().st_mode))
            g_missingCfgDir = szCfgDir;
        else
            cfg::ReadConfigDirectory(szCfgDir, ignoreCfgErrors);
    }

    tStrVec non_opt_args;

    for(auto& keyval : validargs)
    {
        cfg::g_bQuiet = true;
        if(!cfg::SetOption(keyval, 0))
            nonoptions.emplace_back(keyval);
        cfg::g_bQuiet = false;
    }

    cfg::PostProcConfig();

#ifdef DEBUG
    log::g_szLogPrefix = "acngtool";
    log::open();
#endif

    for(const auto& x: nonoptions)
        f(x);
}


std::unordered_map<string, parm> parms =
{
#if 0
   {
        "urltest",
        { 1, 1, [](LPCSTR p)
            {
                std::cout << EncodeBase64Auth(p);
            }
        }
    }
    ,
#endif
#if 0
   {
        "bin2hex",
        { 1, 1, [](LPCSTR p)
            {
         filereader f;
         if(f.OpenFile(p, true))
            exit(EIO);
         std::cout << BytesToHexString(f.GetBuffer(), f.GetSize()) << std::endl;
         exit(EXIT_SUCCESS);
            }
        }
    }
    ,
#endif
#if 0 // def HAVE_DECB64
    {
     "decb64",
     { 1, 1, [](LPCSTR p)
        {
#ifdef DEBUG
           cerr << "decoding " << p <<endl;
#endif
           acbuf res;
           if(DecodeBase64(p, strlen(p), res))
           {
              std::cout.write(res.rptr(), res.size());
              exit(0);
           }
           exit(1);
        }
     }
  }
    ,
#endif
    {
        "encb64",
        { 1, 1, [](LPCSTR p)
            {
#ifdef DEBUG
            cerr << "encoding " << p <<endl;
#endif
                std::cout << EncodeBase64Auth(p);
            }
        }
    }
    ,
        {
            "cfgdump",
            { 0, 0, [](LPCSTR) {
                warn_cfgdir();
                             cfg::dump_config(false);
                         }
            }
        }
    ,
        {
            "curl",
            { 1, UINT_MAX, [](LPCSTR p)
                {
                    if(!p)
                        return;
                    auto ret=wcat(p, getenv("http_proxy"));
                    if(!g_exitCode)
                        g_exitCode = ret;

                }
            }
        },
        {
            "retest",
            {
                1, 1, [](LPCSTR p)
                {
                    warn_cfgdir();
                    static auto matcher = make_shared<rex>();
                    std::cout << ReTest(p, *matcher) << std::endl;
                }
            }
        }
    ,
        {
            "printvar",
            {
                1, 1, [](LPCSTR p)
                {
                    warn_cfgdir();
                    auto ps(cfg::GetStringPtr(p));
                    if(ps) { cout << *ps << endl; return; }
                    auto pi(cfg::GetIntPtr(p));
                    if(pi) {
                        cout << *pi << endl;
                        return;
                    }
                    g_exitCode=23;
                }
            }
        },
        {
            "patch",
            {
                3, 3, [](LPCSTR p)
                {
                    static tStrVec iop;
                    iop.emplace_back(p);
                    if(iop.size() == 3)
                        g_exitCode+=patch_file(iop[0], iop[1], iop[2]);
                }
            }
        }

    ,
        {
            "maint",
            {
                0, 0, [](LPCSTR)
                {
                    warn_cfgdir();
                    g_exitCode+=do_maint_job();
                }
            }
        }
   ,
   {
           "shrink",
           {
                   1, UINT_MAX, [](LPCSTR p)
                   {
                       static bool dryrun(false), apply(false), verbose(false), incIfiles(false);
                       static off_t wantedSize(4000000000);
                       if(!p)
                           g_exitCode += shrink(wantedSize, dryrun, apply, verbose, incIfiles);
                       else if(*p > '0' && *p<='9')
                           wantedSize = strsizeToOfft(p);
                       else if(*p == '-')
                       {
                           for(++p;*p;++p)
                           {
                               if(*p == 'f') apply = true;
                               else if(*p == 'n') dryrun = true;
                               else if (*p == 'x') incIfiles = true;
                               else if (*p == 'v') verbose = true;
                           }
                       }
                   }
           }
   }
};

#endif


#if 0

void do_stuff_before_config()
{
    LPCSTR envvar(nullptr);

    cerr << "Pandora: " << sizeof(regex_t) << endl;
    /*
    // PLAYGROUND
    if (argc < 2)
        return -1;

    acng::cfg:tHostInfo hi;
    cout << "Parsing " << argv[1] << ", result: " << hi.SetUrl(argv[1]) << endl;
    cout << "Host: " << hi.sHost << ", Port: " << hi.sPort << ", Path: "
            << hi.sPath << endl;
    return 0;

    bool Bz2compressFile(const char *, const char*);
    return !Bz2compressFile(argv[1], argv[2]);

    char tbuf[40];
    FormatCurrentTime(tbuf);
    std::cerr << tbuf << std::endl;
    exit(1);
    */
    envvar = getenv("PARSEIDX");
    if (envvar)
    {
        int parseidx_demo(LPCSTR);
        exit(parseidx_demo(envvar));
    }

    envvar = getenv("GETSUM");
    if (envvar)
    {
        uint8_t csum[20];
        string s(envvar);
        off_t resSize;
        bool ok = filereader::GetChecksum(s, CSTYPE_SHA1, csum, false, resSize /*, stdout*/);
        if(!ok)
        {
            perror("");
            exit(1);
        }
        for (unsigned i = 0; i < sizeof(csum); i++)
            printf("%02x", csum[i]);
        printf("\n");
        envvar = getenv("REFSUM");
        if (ok && envvar)
        {
            if(CsEqual(envvar, csum, sizeof(csum)))
            {
                printf("IsOK\n");
                exit(0);
            }
            else
            {
                printf("Diff\n");
                exit(1);
            }
        }
        exit(0);
    }
}

#endif
#if 0
#warning line reader test enabled
    if (cmd == "wcl")
    {
        if (argc < 3)
            usage(2);
        filereader r;
        if (!r.OpenFile(argv[2], true))
        {
            cerr << r.getSErrorString() << endl;
            return EXIT_FAILURE;
        }
        size_t count = 0;
        auto p = r.GetBuffer();
        auto e = p + r.GetSize();
        for (;p < e; ++p)
            count += (*p == '\n');
        cout << count << endl;

        exit(EXIT_SUCCESS);
    }
#endif
#if 0
#warning header parser enabled
    if (cmd == "htest")
    {
        header h;
        h.LoadFromFile(argv[2]);
        cout << string(h.ToString()) << endl;

        h.clear();
        filereader r;
        r.OpenFile(argv[2]);
        std::vector<std::pair<std::string, std::string>> oh;
        h.Load(r.GetBuffer(), r.GetSize(), &oh);
        for(auto& r : oh)
            cout << "X:" << r.first << " to " << r.second;
        exit(0);
    }
#endif
#if 0
#warning benchmark enabled
    if (cmd == "benchmark")
    {
        dump_proc_status_always();
        cfg::g_bQuiet = true;
        cfg::g_bNoComplex = false;
        parse_options(argc - 2, argv + 2, true);
        cfg::PostProcConfig();
        string s;
        tHttpUrl u;
        int res=0;
/*
        acng::cfg:tRepoResolvResult hm;
        tHttpUrl wtf;
        wtf.SetHttpUrl(non_opt_args.front());
        acng::cfg:GetRepNameAndPathResidual(wtf, hm);
*/
        while(cin)
        {
            std::getline(cin, s);
            s += "/xtest.deb";
            if(u.SetHttpUrl(s))
            {
                cfg::tRepoResolvResult xdata;
                cfg::GetRepNameAndPathResidual(u, xdata);
                cout << s << " -> "
                        << (xdata.psRepoName ? "matched" : "not matched")
                        << endl;
            }
        }
        dump_proc_status_always();
        exit(res);
    }
#endif

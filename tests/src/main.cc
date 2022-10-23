#include "gtest/gtest.h"
#include "acres.h"
#include "evabase.h"
#include "main.h"

#include <stdlib.h>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>
#include <locale.h>

using namespace acng;

void pushEvents(int secTimeout, bool* abortVar)
{
	for(auto dateEnd = time(0) + secTimeout;
		time(0) < dateEnd
		&& (abortVar == nullptr || !*abortVar)
		&& ! evabase::GetGlobal().IsShuttingDown()
		;)
	{
		event_base_loop(evabase::base, EVLOOP_NONBLOCK | EVLOOP_ONCE);
	}
}

acres* g_res;

int main(int argc, char **argv)
{
	setlocale(LC_ALL, "C");
	auto p = acng::evabase::Create();
	g_res = acres::Create();

    ::testing::InitGoogleTest(&argc, argv);
	auto r = RUN_ALL_TESTS();
	p->SignalStop();
	pushEvents(2, nullptr);
	return r;
}

namespace acng
{
#ifdef DEBUG
void dbg_handler(evutil_socket_t, short, void*) {}
#endif


class CProcess
{
public:
	pid_t m_forkStatus = 0;
	int m_suc = -1;
	string m_cmd;
	CProcess(std::string cmdLine, bool throughShell) :
		m_cmd(cmdLine)
	{
		m_forkStatus = fork();
		if (m_forkStatus == -1)
		{
			perror("fork");
			exit(EXIT_FAILURE);
		}
		else
		{
			// we are in child context
			if (throughShell)
			{
				//char* parms[] = { "-c", m_cmd.c_str() };
				//m_suc = execvp("sh", parms);
				m_suc = system(m_cmd.c_str());
			}
			else
			{
				char* noarg[] = {0};
				m_suc = execvp(m_cmd.c_str(), noarg);
			}
			exit(EXIT_SUCCESS);
		}
	}
	~CProcess()
	{
		if (m_forkStatus > 0)
		{
			kill(m_forkStatus, SIGTERM);
			kill(m_forkStatus, SIGKILL);
		}
	}
};
}


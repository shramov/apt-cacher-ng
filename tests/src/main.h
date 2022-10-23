#include "gtest/gtest.h"
#include "acres.h"
#include "evabase.h"
//#include <locale.h>

using namespace acng;
using namespace std;

void pushEvents(int secTimeout, bool* abortVar);
extern acres* g_res;

namespace acng
{
#ifdef DEBUG
void dbg_handler(evutil_socket_t, short, void*);
#endif
class conserver;
}

int find_free_port();

struct TFakeServerData
{
    vector<int> ports;
    lint_user_ptr<conserver> serva;
};

TFakeServerData make_fake_server(int nPorts, conserver*p = nullptr);

#include <string>
#include <fstream>
#include <pthread.h>
#include <unordered_map>
#include "cmdline.hpp"
#include "frpcmaster_zjq.hpp"
#include "util_zjq.hpp"
#include "easylogging++.h"
using namespace std;
INITIALIZE_EASYLOGGINGPP

int main(int arc, char* argv[]) {
    Master m = Master::Connect("192.168.0.105",33268);
    m.run();
    return 0;
}
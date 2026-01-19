#include "EchoClient.h"
#include <string.h>
#include <unistd.h>

int main(int argc,char* argv[])
{
    EventLoop loop;
    InetAddress addr;
    EchoClient client{&loop,addr,"EchoClient"};
    client.connect();
    loop.loop();
    return 0;
}
#include <stdio.h>
#include <fcntl.h>

#include <chrono>
#include <thread>

void *start(void *data)
{
    int i;
    size_t idx = (size_t)data;
    for (i=0; i<30; i++)
    {
        if ( idx == 0 )
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return 0;
}

int main(int argc, char const *argv[])
{
    static const size_t nthreads = 16;
    std::thread threads[nthreads];
    size_t i;

    for (i=0; i<nthreads; i++)
        threads[i] = std::move(std::thread(start, (void*)i));

    for (i=0; i<nthreads; i++)
        threads[i].join();
}

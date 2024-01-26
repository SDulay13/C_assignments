#include <cstdio>
#include <cstdlib>

int main() {
    unsigned long n = 0;
    while (fgetc(stdin) != EOF) {
        ++n;
    }
    fprintf(stdout, "%8lu\n", n);
}
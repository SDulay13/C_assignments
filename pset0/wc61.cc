#include <cstdio>
#include <cctype>

int main() {
    unsigned long cc = 0, wc = 0, lc = 0;

    while (true) {
        int ch = fgetc(stdin);

        if (ch == (' ') or ch == '\n') {
            ++wc;
        }
            
        if (ch == '\n'){
            ++lc;
        }
        if (ch==EOF){
            break;
        }
        ++cc; 
    }
    fprintf(stdout, "%8lu %7lu %7lu\n", cc, wc, lc);
}
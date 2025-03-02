#include <cstring>
#include <thornhill>

#include "drivers/hardware/serial.hpp"

using namespace std;
using namespace Thornhill;

namespace Thornhill::Kernel {

    void printChar(char c) { ThornhillSerial::writeCharacter(c); }
    void print(const char* message, bool appendNewline) {
        ThornhillSerial::write(message, appendNewline);
    }

    int vprintf(const char* fmt, va_list arg) {
        int length = 0;

        char* strPtr;
        char buffer[32];

        char c;
        while ((c = *fmt++)) {

            if ('%' == c) {
                switch ((c = *fmt++)) {

                    /* %% => print a single % symbol (escape) */
                    case '%':
                        printChar('%');
                        length++;
                        break;

                    /* %c => print a character */
                    case 'c':
                        printChar((char) va_arg(arg, int));
                        length++;
                        break;

                    /* %s => print a string */
                    case 's':
                        strPtr = va_arg(arg, char*);
                        print(strPtr, false);
                        length += strlen(strPtr);
                        break;

                    /* %d => print number as decimal */
                    case 'd':
                        itoa(buffer, va_arg(arg, int64_t), 10, 32);
                        print(buffer, false);
                        length += strlen(buffer);
                        break;

                    /* %u => print unsigned number as integer */
                    case 'u':
                        uitoa(buffer, va_arg(arg, uint64_t), 10, 32);
                        print(buffer, false);
                        length += strlen(buffer);
                        break;

                    /* %x => print number as hexadecimal */
                    case 'x':
                        uitoa(buffer, va_arg(arg, uint64_t), 16, 32);
                        print(buffer, false);
                        length += strlen(buffer);
                        break;

                    /* %n => print newline */
                    case 'n':
                        printChar('\r');
                        printChar('\n');
                        length += 2;
                        break;

                }
            } else {
                printChar(c);
                length++;
            }
        }

        return length;
    }

    void printf(const char* fmt...) {
        va_list arg;

        va_start(arg, fmt);
        vprintf(fmt, arg);
        va_end(arg);
    }

}
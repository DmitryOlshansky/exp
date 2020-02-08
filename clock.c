#if 0
PROG=/tmp/$(md5sum $0 | awk '{ print $1 }')
if ! [ -x $PROG ] ; then
    gcc -Wall -o $PROG $0 -lm|| exit 1
fi
exec $PROG "$@"

#endif
/*
    clock.c - a simple LED clock working with Gyver TM1367
    2-wire LED controller module
*/
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

typedef uint8_t u8;

enum {
    AUTO_ADDR = 0x40,
    FIXED_ADDR = 0x44,
    START_ADDR = 0xc0,
    DIO_PIN = 252,  // set to your IO pin as in /sys/class/gpio/gpioXYZ
    CLK_PIN = 253,  // set to your CLK pin as in /sys/class/gpio/gpioXYZ
    LOW = 0,
    HIGH = 1
};

const char* INPUT = "in";
const char* OUTPUT = "out";

void error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    exit(1);
}

void exportPin(int n) {
    char buf[80];
    int exp = open("/sys/class/gpio/export", O_WRONLY);
    if (exp < 0) error("Failed to open gpio export: %s\n", strerror(errno));
    int chars = snprintf(buf, sizeof(buf), "%d\n", n);
    write(exp, buf, chars);
    close(exp);
}


void digitalWrite(int pin, int value) {
    char buf[80];
    snprintf(buf, sizeof(buf), "/sys/class/gpio/gpio%d/value", pin);
    int fd = open(buf, O_RDWR);
    if (fd < 0) error("Failed to open pin value file: %s\n", strerror(errno));

    int chars = snprintf(buf, sizeof(buf), "%d\n", value);
    int resp = write(fd, buf, chars);
    close(fd);
    
    if (resp != chars) error("Failed to write to pin - %s\n", strerror(errno));
}

int digitalRead(int pin) {
    char buf[80];
    snprintf(buf, sizeof(buf), "/sys/class/gpio/gpio%d/value", pin);
    int fd = open(buf, O_RDWR);
    if (fd < 0) error("Failed to open pin value file: %s\n", strerror(errno));
    int value;
    int resp = read(fd, buf, sizeof(buf));
    close(fd);
    if (resp <= 0) error("Failed to read from pin: %s", strerror(errno));
    if (sscanf(buf, "%d", &value) != 1) error("failed to parse pin data\n");
    return value;
}

void pinMode(int pin, const char* mode) {
    char buf[80];
    snprintf(buf, sizeof(buf), "/sys/class/gpio/gpio%d/direction", pin);
    int fd = open(buf, O_RDWR);
    if (fd < 0) error("Failed to open pin mode file: %s\n", strerror(errno));
    int sz = strlen(mode);
    int resp = write(fd, mode, sz);
    close(fd);
    if (resp != sz) error("Failed to set pin mode: %s\n", strerror(errno));
}

void delay(double sec) {
    struct timespec ts;
    ts.tv_nsec = round((sec - trunc(sec))*1e9);
    ts.tv_sec = trunc(sec);
    nanosleep(&ts, NULL);
}

void delayUs(int us) {
    delay(us*1e-6);
}

int writeByte(u8 wr_data)
{
	int i;
	for (i = 0; i < 8; i++) //send 8bit data
	{
		digitalWrite(CLK_PIN, LOW);
		digitalWrite(DIO_PIN, wr_data & 0x01); //LSB first
		wr_data >>= 1;
		digitalWrite(CLK_PIN, HIGH);
        delayUs(1);
	}
	digitalWrite(CLK_PIN, LOW); //wait for the ACK
	digitalWrite(DIO_PIN, HIGH);
	digitalWrite(CLK_PIN, HIGH);
	pinMode(DIO_PIN, INPUT);
	delayUs(50);

	uint8_t ack = digitalRead(DIO_PIN);
	if (ack == 0)
	{
		pinMode(DIO_PIN, OUTPUT);
		digitalWrite(DIO_PIN, LOW);
	}
	delayUs(50);
	pinMode(DIO_PIN, OUTPUT);
	delayUs(50);

	return ack;
}

// Send start signal to GyverTM1637
void start()
{
	digitalWrite(CLK_PIN, HIGH);
	digitalWrite(DIO_PIN, HIGH);
	digitalWrite(DIO_PIN, LOW);
	digitalWrite(CLK_PIN, LOW);
}

// End of transmission
void stop() {
	digitalWrite(CLK_PIN, LOW);
	digitalWrite(DIO_PIN, LOW);
	digitalWrite(CLK_PIN, HIGH);
	digitalWrite(DIO_PIN, HIGH);
}

// cell - [0, 3]
// data - display mask of segments
void display(u8 cell, u8 data) {
    u8 brightness = 0x7;
    start();          
	writeByte(FIXED_ADDR);
	stop();
	start();
	writeByte(cell | START_ADDR);
	writeByte(data);
	stop();
	start();
	writeByte(0x88 + brightness);
	stop();
}

// segments are clock-wise, with last one in the middle
const u8 digit2mask[] = {
    0x3f,   // 0
    0x06,   // 1
    0x5B,   // 2
    0x4f,   // 3
    0x02 + 0x04 + 0x20 + 0x40, // 4
    0x01 + 0x04 + 0x08 + 0x20 + 0x40, // 5
    ~0x02, // 6
    0x01 + 0x02 + 0x04, // 7 
    0x7f,    // 8
    0x1 + 0x02 + 0x04 + 0x08 + 0x20 + 0x40    // 9
};

void displayTime(int hours, int minutes, int points) {
    int mask = points ? 0x80 : 0x0;
    display(0x0, digit2mask[hours / 10] | mask);
    display(0x1, digit2mask[hours % 10] | mask);
    display(0x2, digit2mask[minutes / 10] | mask);
    display(0x3, digit2mask[minutes % 10] | mask);
}

int main() {
    exportPin(DIO_PIN);
    exportPin(CLK_PIN);
    pinMode(CLK_PIN, OUTPUT);
    pinMode(DIO_PIN, OUTPUT);
    int points = 0;
    for(;;) {
        time_t ts = time(NULL);
        const struct tm *t = localtime(&ts);
        displayTime(t->tm_hour, t->tm_min, points);
        delay(0.75);
        points = !points;
    }
    return 0;
}

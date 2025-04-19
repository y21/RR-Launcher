#include <fat.h>
#include <stdio.h>
#include <ogc/system.h>
#include <unistd.h>

char *foos()
{
	return "hello";
}
extern u8 __stack_addr[], __stack_end[];

int main()
{
	SYS_Report("Stack addr = %x, Stack end = %x\n", __stack_addr, __stack_end);
	char *r = foos();
	SYS_Report("%x %s\n", r, r);

	bool res = fatInitDefault();
	SYS_Report("Init = %d\n", res);

	FILE *file = fopen("RetroRewind6/version.txt", "r");
	char buf[32];
	int read = fread(buf, 1, 32, file);
	SYS_Report("Read = %d\nFile = %s\n", read, buf);

	usleep(5000000);
}



#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>


#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <linux/input.h>
#include <linux/uinput.h>

#define GND -1
struct {
	int pin;
	int key;
} *io,

	
   ioStandard[] = {

	{  25,     KEY_LEFT     },   
	{   9,     KEY_RIGHT    },
	{  10,     KEY_UP       },
	{  17,     KEY_DOWN     },
	{  -1,     -1           } }; // end


char
  *progName,                         // Program name (for error reporting)
   sysfs_root[] = "/sys/class/gpio", // Location of Sysfs GPIO files
   running      = 1;                 // Signal handler will set to 0 (exit)
volatile unsigned int
  *gpio;                             // GPIO register table
const int
   debounceTime = 20;                // 20 ms for button debouncing



int pinConfig(int pin, char *attr, char *value) {
	char filename[50];
	int  fd, w, len = strlen(value);
	sprintf(filename, "%s/gpio%d/%s", sysfs_root, pin, attr);
	if((fd = open(filename, O_WRONLY)) < 0) return -1;
	w = write(fd, value, len);
	close(fd);
	return (w != len); // 0 = success
}


void cleanup() {
	char buf[50];
	int  fd, i;
	sprintf(buf, "%s/unexport", sysfs_root);
	if((fd = open(buf, O_WRONLY)) >= 0) {
		for(i=0; io[i].pin >= 0; i++) {
			// Restore GND items to inputs
			if(io[i].key == GND)
				pinConfig(io[i].pin, "direction", "in");
			// And un-export all items regardless
			sprintf(buf, "%d", io[i].pin);
			write(fd, buf, strlen(buf));
		}
		close(fd);
	}
}


void err(char *msg) {
	printf("%s: %s.  Try 'sudo %s'.\n", progName, msg, progName);
	cleanup();
	exit(1);
}

// Interrupt handler -- set global flag to abort main loop.
void signalHandler(int n) {
	running = 0;
}

 
static int boardType(void) {     //Expected Model B 3 for our board 
	FILE *fp;
	char  buf[1024], *ptr;
	int   n, board = 1;


#if 1
	if((fp = fopen("/proc/cmdline", "r"))) {
		while(fgets(buf, sizeof(buf), fp)) {
			if((ptr = strstr(buf, "mem_size=")) &&
			   (sscanf(&ptr[9], "%x", &n) == 1) &&
			   (n == 0x3F000000)) {
				board = 2; // Appears to be a Pi 2
				break;
			} else if((ptr = strstr(buf, "boardrev=")) &&
			          (sscanf(&ptr[9], "%x", &n) == 1) &&
			          ((n == 0x02) || (n == 0x03))) {
				board = 0; // Appears to be an early Pi
				break;
			}
		}
		fclose(fp);
	}
#else
	char s[8];
	if((fp = fopen("/proc/cpuinfo", "r"))) {
		while(fgets(buf, sizeof(buf), fp)) {
			if((ptr = strstr(buf, "Hardware")) &&
			   (sscanf(&ptr[8], " : %7s", s) == 1) &&
			   (!strcmp(s, "BCM2709"))) {
				board = 2; // Appears to be a Pi 2
				break;
			} else if((ptr = strstr(buf, "Revision")) &&
			          (sscanf(&ptr[8], " : %x", &n) == 1) &&
			          ((n == 0x02) || (n == 0x03))) {
				board = 0; // Appears to be an early Pi
				break;
			}
		}
		fclose(fp);
	}
#endif

	return board;
}

// Main

#define PI1_BCM2708_PERI_BASE 0x20000000
#define PI1_GPIO_BASE         (PI1_BCM2708_PERI_BASE + 0x200000)
#define PI2_BCM2708_PERI_BASE 0x3F000000
#define PI2_GPIO_BASE         (PI2_BCM2708_PERI_BASE + 0x200000)
#define BLOCK_SIZE            (4*1024)
#define GPPUD                 (0x94 / 4)
#define GPPUDCLK0             (0x98 / 4)

int main(int argc, char *argv[]) {


	char                   buf[50],     
	                       c,            
	                       board;        
	int                    fd,           
	                       i, j,        
	                       bitmask,      
	                       timeout = -1, 
	                       intstate[32], 
	                       extstate[32], 
	                       lastKey = -1; 
	unsigned long          bitMask, bit; 
	volatile unsigned char shortWait;    
	struct input_event     keyEv, synEv; 
	struct pollfd          p[32];        // GPIO

	progName = argv[0];      
	signal(SIGINT , signalHandler); 
	signal(SIGKILL, signalHandler);
.
	io = (access("/etc/modprobe.d/adafruit.conf", F_OK) ||
	      access("/dev/fb1", F_OK)) ? ioStandard : ioTFT;


	if((fd = open("/dev/mem", O_RDWR | O_SYNC)) < 0)
		err("Can't open /dev/mem");
	gpio = mmap(            // Memory-mapped I/O
	  NULL,                 // Any adddress will do
	  BLOCK_SIZE,           // Mapped block length
	  PROT_READ|PROT_WRITE, // Enable read+write
	  MAP_SHARED,           // Shared with other processes
	  fd,                   // File to map
	  (board == 2) ?
	   PI2_GPIO_BASE :      // -> GPIO registers
	   PI1_GPIO_BASE);

	close(fd);              
	if(gpio == MAP_FAILED) err("Can't mmap()");
	// Make combined bitmap of pullup-enabled pins:
	for(bitmask=i=0; io[i].pin >= 0; i++)
		if(io[i].key != GND) bitmask |= (1 << io[i].pin);
	gpio[GPPUD]     = 2;                  
	for(shortWait=150;--shortWait;);        
	gpio[GPPUDCLK0] = bitmask;             
	for(shortWait=150;--shortWait;);       
	gpio[GPPUD]     = 0;                   
	gpio[GPPUDCLK0] = 0;
	(void)munmap((void *)gpio, BLOCK_SIZE);


	sprintf(buf, "%s/export", sysfs_root);
	
	if((fd = open(buf, O_WRONLY)) < 0) 
		err("Can't open GPIO export file");
		
	for(i=j=0; io[i].pin >= 0; i++) { 
		sprintf(buf, "%d", io[i].pin);
		write(fd, buf, strlen(buf));             
		pinConfig(io[i].pin, "active_low", "0"); 
		
		if(io[i].key == GND) {
			// Set pin to output, value 0
			
			if(pinConfig(io[i].pin, "direction", "out") ||
			   pinConfig(io[i].pin, "value"    , "0"))
				err("Pin config failed (GND)");
				
		} 
		else {

			if(pinConfig(io[i].pin, "direction", "in") ||
			   pinConfig(io[i].pin, "edge"     , "both"))
				err("Pin config failed");

			sprintf(buf, "%s/gpio%d/value",
			  sysfs_root, io[i].pin);

			if((p[j].fd = open(buf, O_RDONLY)) < 0)
				err("Can't access pin value");
			
			intstate[j] = 0;
			if((read(p[j].fd, &c, 1) == 1) && (c == '0'))
				intstate[j] = 1;
			
			extstate[j] = intstate[j];
			p[j].events  = POLLPRI; // Set up poll() events
			p[j].revents = 0;
			j++;
			
		} //ends else 
	}  //ends for 


	if((fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK)) < 0)
		err("Can't open /dev/uinput");
	
	if(ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0)
		err("Can't SET_EVBIT");
	
	for(i=0; io[i].pin >= 0; i++) {
		
		if(io[i].key != GND) {
			
			if(ioctl(fd, UI_SET_KEYBIT, io[i].key) < 0)
				err("Can't SET_KEYBIT");
		
		}
	} //Ends for 
	
	
	if(ioctl(fd, UI_SET_KEYBIT, vulcanKey) < 0) err("Can't SET_KEYBIT");
	struct uinput_user_dev uidev;
	memset(&uidev, 0, sizeof(uidev));
	snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "retrogame");
	uidev.id.bustype = BUS_USB;
	uidev.id.vendor  = 0x1;
	uidev.id.product = 0x1;
	uidev.id.version = 1;
	if(write(fd, &uidev, sizeof(uidev)) < 0)
		err("write failed");
	if(ioctl(fd, UI_DEV_CREATE) < 0)
		err("DEV_CREATE failed");
#else 
	if((fd = open("/dev/input/event0", O_WRONLY | O_NONBLOCK)) < 0)
		err("Can't open /dev/input/event0");
#endif

	memset(&keyEv, 0, sizeof(keyEv));
	keyEv.type  = EV_KEY;
	memset(&synEv, 0, sizeof(synEv));
	synEv.type  = EV_SYN;
	synEv.code  = SYN_REPORT;
	synEv.value = 0;

	
	while(running) { 

		if(poll(p, j, timeout) > 0) { 
			for(i=0; i<j; i++) {       
				if(p[i].revents) { 

					lseek(p[i].fd, 0, SEEK_SET);
					read(p[i].fd, &c, 1);
					if(c == '0')      intstate[i] = 1;
					else if(c == '1') intstate[i] = 0;
					p[i].revents = 0; // Clear flag
				}
			}
			timeout = debounceTime; 
			c       = 0;           
			// Else timeout occurred
		} else if(timeout == debounceTime) { 
		
			bitMask = 0L; // Mask of buttons currently pressed
			bit     = 1L;
			for(c=i=j=0; io[i].pin >= 0; i++, bit<<=1) {
				if(io[i].key != GND) {
					
					if(intstate[j] != extstate[j]) {
						extstate[j] = intstate[j];
						keyEv.code  = io[i].key;
						keyEv.value = intstate[j];
						write(fd, &keyEv,
						  sizeof(keyEv));
						c = 1; 
						if(intstate[j]) {

							lastKey = i;
							timeout = repTime1;
						} else { // Release?
							// Stop repeat and
							// return to normal
							// IRQ monitoring
							// (no timeout).
							lastKey = timeout = -1;
						}
					}
					j++;
					if(intstate[i]) bitMask |= bit;
				}
			}

			if((bitMask & vulcanMask) == vulcanMask)
				timeout = vulcanTime;
			
		} 
		else if(timeout == vulcanTime) { 
			keyEv.code = vulcanKey;
			for(i=1; i>= 0; i--) { 
				keyEv.value = i;
				write(fd, &keyEv, sizeof(keyEv));
				usleep(10000); 
				write(fd, &synEv, sizeof(synEv));
				usleep(10000);
			} //ends for 
			
			timeout = -1; 
			c       = 0;  
		} 
		else if(lastKey >= 0) { 
			if(timeout == repTime1) timeout = repTime2;
			else if(timeout > 30)   timeout -= 5; 
			c           = 1; 
			keyEv.code  = io[lastKey].key;
			keyEv.value = 2; 
			write(fd, &keyEv, sizeof(keyEv));
		}
		if(c) write(fd, &synEv, sizeof(synEv));
	}


	ioctl(fd, UI_DEV_DESTROY); // Destroy and
	close(fd);                 // close uinput
	cleanup();                 // Un-export pins

	puts("Done.");

	return 0;
	
	
} //Final
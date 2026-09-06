/* Synthetic test: no device access. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../overlay/package/xr500v-voip/src/dtmf.h"
static void tone(int16_t *s, double a, double b, int offset, double amplitude, double second)
{
	for (int i=0;i<160;i++) s[i] = 900 + amplitude * sin(2*M_PI*a*(i+offset)/8000) + second * sin(2*M_PI*b*(i+offset)/8000);
}
int main(void)
{
	double rows[]={697,770,852,941}, cols[]={1209,1336,1477,1633};
	char keys[]="123A456B789C*0#D";
	int16_t s[160], silence[160]={0};
	for(int k=0;k<16;k++) {
		for(int phase=0;phase<160;phase+=17) {
			struct dtmf_detector d={0};
			for(int n=0;n<12;n++) {
				tone(s,rows[k/4],cols[k%4],phase+n*160,2200,2200);
				char c=dtmf_feed(&d,s);
				assert(c==(n==1?keys[k]:0));
			}
			assert(!dtmf_feed(&d,silence));assert(!dtmf_feed(&d,silence));
			tone(s,rows[k/4],cols[k%4],0,700,1000);
			assert(!dtmf_feed(&d,s));assert(dtmf_feed(&d,s)==keys[k]);
		}
	}
	for(int n=0;n<1000;n++) {
		tone(s,425,0,n*160,3000,0);assert(!dtmf_window(s));
		tone(s,697,0,n*160,3000,0);assert(!dtmf_window(s));
		unsigned rng=n+1;
		for(int i=0;i<160;i++){rng=rng*1664525+1013904223;s[i]=(int16_t)(rng>>16)/4;}
		assert(!dtmf_window(s));
	}
	puts("PASS: all 16 tones, phase offsets, DC offset, held-key/repeat debounce, dial tone/single-tone/noise rejection");
}

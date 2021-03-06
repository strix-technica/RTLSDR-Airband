/*
 * RTLSDR AM/NFM demodulator, mixer, streamer and recorder
 *
 * Copyright (c) 2014 Wong Man Hang <microtony@gmail.com>
 * Copyright (c) 2015-2016 Tomasz Lemiech <szpajder@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#if defined USE_BCM_VC && !defined __arm__
#error Broadcom VideoCore support can only be enabled on ARM builds
#endif

// From this point we may safely assume that USE_BCM_VC implies __arm__

#if defined (__arm__) || defined (__aarch64__)

#ifdef USE_BCM_VC
#include "hello_fft/mailbox.h"
#include "hello_fft/gpu_fft.h"
#else
#include <fftw3.h>
#endif

#else	/* x86 */
#include <xmmintrin.h>
#include <fftw3.h>

#endif	/* x86 */

#include <unistd.h>
#include <pthread.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <algorithm>
#include <csignal>
#include <cstdarg>
#include <cerrno>
#include <iostream>
#include <cstring>
#include <cstdio>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <libconfig.h++>
#include <ogg/ogg.h>
#include <vorbis/vorbisenc.h>
#include <shout/shout.h>
#include <lame/lame.h>
#include <rtl-sdr.h>
#include "rtl_airband.h"

using namespace std;
using namespace libconfig;

device_t* devices;
mixer_t* mixers;
int device_count, mixer_count;
volatile int device_opened = 0;
int rtlsdr_buffers = 10;
int foreground = 0, do_syslog = 1, shout_metadata_delay = 3;
volatile int do_exit = 0;
bool use_localtime = false;
#ifdef NFM
float alpha = exp(-1.0f/(WAVE_RATE * 2e-4));
enum fm_demod_algo {
	FM_FAST_ATAN2,
	FM_QUADRI_DEMOD
};
enum fm_demod_algo fm_demod = FM_FAST_ATAN2;
#endif
#if DEBUG
char *debug_path;
#endif
pthread_cond_t	mp3_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t	mp3_mutex = PTHREAD_MUTEX_INITIALIZER;

void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx) {
	if(do_exit) return;
	device_t *dev = (device_t*)ctx;
	pthread_mutex_lock(&dev->buffer_lock);
	memcpy(dev->buffer + dev->bufe, buf, len);
	if (dev->bufe == 0) {
		memcpy(dev->buffer + BUF_SIZE, buf, FFT_SIZE * 2);
	}
	dev->bufe = dev->bufe + len;
	if (dev->bufe == BUF_SIZE) dev->bufe = 0;
	pthread_mutex_unlock(&dev->buffer_lock);
}

void sighandler(int sig) {
	log(LOG_NOTICE, "Got signal %d, exiting\n", sig);
	do_exit = 1;
}

void* rtlsdr_exec(void* params) {
	int r;
	device_t *dev = (device_t*)params;

	dev->rtlsdr = NULL;
	rtlsdr_open(&dev->rtlsdr, dev->device);

	if (NULL == dev->rtlsdr) {
		log(LOG_ERR, "Failed to open rtlsdr device #%d.\n", dev->device);
		error();
		return NULL;
	}
	r = rtlsdr_set_sample_rate(dev->rtlsdr, SOURCE_RATE);
	if (r < 0) log(LOG_ERR, "Failed to set sample rate for device #%d. Error %d.\n", dev->device, r);
	r = rtlsdr_set_center_freq(dev->rtlsdr, dev->centerfreq);
	if (r < 0) log(LOG_ERR, "Failed to set center freq for device #%d. Error %d.\n", dev->device, r);
	r = rtlsdr_set_freq_correction(dev->rtlsdr, dev->correction);
	if (r < 0 && r != -2 ) log(LOG_ERR, "Failed to set freq correction for device #%d. Error %d.\n", dev->device, r);

	r = rtlsdr_set_tuner_gain_mode(dev->rtlsdr, 1);
	r |= rtlsdr_set_tuner_gain(dev->rtlsdr, dev->gain);
	if (r < 0)
		log(LOG_ERR, "Failed to set gain to %0.2f for device #%d. Error %d.\n", (float)rtlsdr_get_tuner_gain(dev->rtlsdr) / 10.0, dev->device, r);
	else
		log(LOG_INFO, "Device #%d: gain set to %0.2f dB\n", dev->device, (float)rtlsdr_get_tuner_gain(dev->rtlsdr) / 10.0);

	r = rtlsdr_set_agc_mode(dev->rtlsdr, 0);
	if (r < 0) log(LOG_ERR, "Failed to disable AGC for device #%d. Error %d.\n", dev->device, r);
	rtlsdr_reset_buffer(dev->rtlsdr);
	log(LOG_INFO, "Device %d started.\n", dev->device);
	atomic_inc(&device_opened);
	dev->failed = 0;
	if(rtlsdr_read_async(dev->rtlsdr, rtlsdr_callback, params, rtlsdr_buffers, 320000) < 0) {
		log(LOG_WARNING, "Device #%d: async read failed, disabling\n", dev->device);
		dev->failed = 1;
		disable_device_outputs(dev);
		atomic_dec(&device_opened);
	}
	return 0;
}

void* controller_thread(void* params) {
	device_t *dev = (device_t*)params;
	int i = 0;
	int consecutive_squelch_off = 0;
	struct timeval tv;

	if(dev->channels[0].freq_count < 2) return 0;
	while(!do_exit) {
		SLEEP(200);
		if(dev->channels[0].axcindicate == ' ') {
			if(consecutive_squelch_off < 10) {
				consecutive_squelch_off++;
			} else {
				i++; i %= dev->channels[0].freq_count;
				dev->channels[0].frequency = dev->channels[0].freqlist[i];
				dev->centerfreq = dev->channels[0].freqlist[i] + 2 * (double)(SOURCE_RATE / FFT_SIZE);
				rtlsdr_set_center_freq(dev->rtlsdr, dev->centerfreq);
			}
		} else {
			if(consecutive_squelch_off == 10) {
				if(i != dev->last_frequency) {
// squelch has just opened on a new frequency - we might need to update outputs' metadata
					gettimeofday(&tv, NULL);
					tag_queue_put(dev, i, tv);
					dev->last_frequency = i;
				}
			}
			consecutive_squelch_off = 0;
		}
	}
	return 0;
}

#ifdef NFM
void multiply(float ar, float aj, float br, float bj, float *cr, float *cj)
{
	*cr = ar*br - aj*bj;
	*cj = aj*br + ar*bj;
}

float fast_atan2(float y, float x)
{
	float yabs, angle;
	float pi4=M_PI_4, pi34=3*M_PI_4;
	if (x==0.0f && y==0.0f) {
		return 0;
	}
	yabs = y;
	if (yabs < 0.0f) {
		yabs = -yabs;
	}
	if (x >= 0.0f) {
		angle = pi4 - pi4 * (x-yabs) / (x+yabs);
	} else {
		angle = pi34 - pi4 * (x+yabs) / (yabs-x);
	}
	if (y < 0.0f) {
		return -angle;
	}
	return angle;
}

float polar_disc_fast(float ar, float aj, float br, float bj)
{
	float cr, cj;
	multiply(ar, aj, br, -bj, &cr, &cj);
	return (float)(fast_atan2(cj, cr) * M_1_PI);
}

float fm_quadri_demod(float ar, float aj, float br, float bj) {
	return (float)((br*aj - ar*bj)/(ar*ar + aj*aj + 1.0f) * M_1_PI);
}

#endif

class AFC
{
	const char _prev_axcindicate;

#ifdef USE_BCM_VC
	float square(const GPU_FFT_COMPLEX *fft_results, int index)
	{
		return fft_results[index].re * fft_results[index].re + fft_results[index].im * fft_results[index].im;
	}
#else
	float square(const fftwf_complex *fft_results, int index)
	{
		return fft_results[index][0] * fft_results[index][0] + fft_results[index][1] * fft_results[index][1];
	}
#endif
	template <class FFT_RESULTS, int STEP>
		int check(const FFT_RESULTS* fft_results, const int base, const float base_value, unsigned char afc)
	{
		float threshold = 0;
		int bin;
		for (bin = base;; bin+= STEP) {
			if (STEP < 0) {
			if (bin < -STEP)
				break;

			} else if ( (bin + STEP) >= FFT_SIZE)
				break;

			const float value = square(fft_results, bin + STEP);
			if (value <= base_value)
				break;

			if (base == bin) {
				threshold = (value - base_value) / (float)afc;
			} else {
				if ((value - base_value) < threshold)
					break;

				threshold+= threshold / 10.0;
			}
		}
		return bin;
	}

public:
	AFC(device_t* dev, int index) : _prev_axcindicate(dev->channels[index].axcindicate)
	{
	}

	template <class FFT_RESULTS>
		void finalize(device_t* dev, int index, const FFT_RESULTS* fft_results)
	{
		channel_t *channel = &dev->channels[index];
		if (channel->afc==0)
			return;

		const char axcindicate = channel->axcindicate;
		if (axcindicate != ' ' && _prev_axcindicate == ' ') {
			const int base = dev->base_bins[index];
			const float base_value = square(fft_results, base);
			int bin = check<FFT_RESULTS, -1>(fft_results, base, base_value, channel->afc);
			if (bin == base)
				bin = check<FFT_RESULTS, 1>(fft_results, base, base_value, channel->afc);

			if (dev->bins[index] != bin) {
#ifdef AFC_LOGGING
				log(LOG_INFO, "AFC device=%d channel=%d: base=%d prev=%d now=%d\n", dev->device, index, base, dev->bins[index], bin);
#endif
				dev->bins[index] = bin;
				if ( bin > base )
					channel->axcindicate = '>';
				else if ( bin < base )
					channel->axcindicate = '<';
			}
		}
		else if (axcindicate == ' ' && _prev_axcindicate != ' ')
			dev->bins[index] = dev->base_bins[index];
	}
};

void demodulate() {

	// initialize fft engine
#ifndef USE_BCM_VC
	fftwf_plan fft;
	fftwf_complex* fftin;
	fftwf_complex* fftout;
	fftin = fftwf_alloc_complex(FFT_SIZE);
	fftout = fftwf_alloc_complex(FFT_SIZE);
	fft = fftwf_plan_dft_1d(FFT_SIZE, fftin, fftout, FFTW_FORWARD, FFTW_MEASURE);
#else
	int mb = mbox_open();
	struct GPU_FFT *fft;
	int ret = gpu_fft_prepare(mb, FFT_SIZE_LOG, GPU_FFT_FWD, FFT_BATCH, &fft);
	switch (ret) {
		case -1: log(LOG_CRIT, "Unable to enable V3D. Please check your firmware is up to date.\n"); error();
		case -2: log(LOG_CRIT, "log2_N=%d not supported. Try between 8 and 17.\n", FFT_SIZE_LOG); error();
		case -3: log(LOG_CRIT, "Out of memory. Try a smaller batch or increase GPU memory.\n"); error();
	}
#endif

#ifdef NFM
	float rotated_r, rotated_j;
#endif
	ALIGN float ALIGN2 levels[256];
	for (int i=0; i<256; i++) {
		levels[i] = i-127.5f;
	}

	// initialize fft window
	// blackman 7
	// the whole matrix is computed
	ALIGN float ALIGN2 window[FFT_SIZE * 2];
	const double a0 = 0.27105140069342f;
	const double a1 = 0.43329793923448f;	const double a2 = 0.21812299954311f;
	const double a3 = 0.06592544638803f;	const double a4 = 0.01081174209837f;
	const double a5 = 0.00077658482522f;	const double a6 = 0.00001388721735f;

	for (int i = 0; i < FFT_SIZE; i++) {
		double x = a0 - (a1 * cos((2.0 * M_PI * i) / (FFT_SIZE-1)))
			+ (a2 * cos((4.0 * M_PI * i) / (FFT_SIZE - 1)))
			- (a3 * cos((6.0 * M_PI * i) / (FFT_SIZE - 1)))
			+ (a4 * cos((8.0 * M_PI * i) / (FFT_SIZE - 1)))
			- (a5 * cos((10.0 * M_PI * i) / (FFT_SIZE - 1)))
			+ (a6 * cos((12.0 * M_PI * i) / (FFT_SIZE - 1)));
		window[i * 2] = window[i * 2 + 1] = (float)x;
	}

	struct timeval ts, te;
	if(DEBUG)
		gettimeofday(&ts, NULL);
	// speed2 = number of bytes per wave sample (x 2 for I and Q)
	int speed2 = (SOURCE_RATE * 2) / WAVE_RATE;
	int device_num = 0;
	while (true) {

		if(do_exit) {
#ifdef USE_BCM_VC
			log(LOG_INFO, "Freeing GPU memory\n");
			gpu_fft_release(fft);
#endif
			return;
		}

		device_t* dev = devices + device_num;
		pthread_mutex_lock(&dev->buffer_lock);
		int available = dev->bufe - dev->bufs;
		if (available < 0) {
			available += BUF_SIZE;
		}
		pthread_mutex_unlock(&dev->buffer_lock);

		if(atomic_get(&device_opened)==0) {
			log(LOG_ERR, "All receivers failed, exiting\n");
			do_exit = 1;
			continue;
		}
		if (dev->failed) {
			// move to next device
			device_num = (device_num + 1) % device_count;
			continue;
		} else if (available < speed2 * FFT_BATCH + FFT_SIZE * 2) {
			// move to next device
			device_num = (device_num + 1) % device_count;
			SLEEP(10);
			continue;
		}

#if defined USE_BCM_VC
		sample_fft_arg sfa = {FFT_SIZE / 4, fft->in};
		for (int i = 0; i < FFT_BATCH; i++) {
			samplefft(&sfa, dev->buffer + dev->bufs + i * speed2, window, levels);
			sfa.dest+= fft->step;
		}
#elif defined (__arm__) || defined (__aarch64__)
		for (int i = 0; i < FFT_SIZE; i++) {
			unsigned char* buf2 = dev->buffer + dev->bufs + i * 2;
			fftin[i][0] = levels[*(buf2)] * window[i*2];
			fftin[i][1] = levels[*(buf2+1)] * window[i*2];
		}
#else /* x86 */
		for (int i = 0; i < FFT_SIZE; i += 2) {
			unsigned char* buf2 = dev->buffer + dev->bufs + i * 2;
			__m128 a = _mm_set_ps(levels[*(buf2 + 3)], levels[*(buf2 + 2)], levels[*(buf2 + 1)], levels[*(buf2)]);
			__m128 b = _mm_load_ps(&window[i * 2]);
			a = _mm_mul_ps(a, b);
			_mm_store_ps(&fftin[i][0], a);
		}
#endif
#ifdef USE_BCM_VC
		gpu_fft_execute(fft);
#else
		fftwf_execute(fft);
#endif

#ifdef USE_BCM_VC
		for (int i = 0; i < dev->channel_count; i++) {
			float *wavein = dev->channels[i].wavein + dev->waveend;
			__builtin_prefetch(wavein, 1);
			const int bin = dev->bins[i];
			const GPU_FFT_COMPLEX *fftout = fft->out + bin;
			for (int j = 0; j < FFT_BATCH; j++, ++wavein, fftout+= fft->step)
				*wavein = sqrtf(fftout->im * fftout->im + fftout->re * fftout->re);
		}
# ifdef NFM
		for (int j = 0; j < dev->channel_count; j++) {
			if(dev->channels[j].modulation == MOD_NFM) {
				struct GPU_FFT_COMPLEX *ptr = fft->out;
				for (int job = 0; job < FFT_BATCH; job++) {
					dev->channels[j].complex_samples[2*(dev->waveend+job)] = ptr[dev->bins[j]].re;
					dev->channels[j].complex_samples[2*(dev->waveend+job)+1] = ptr[dev->bins[j]].im;
					ptr += fft->step;
				}
			}
		}
# endif // NFM
#else
		for (int j = 0; j < dev->channel_count; j++) {
			dev->channels[j].wavein[dev->waveend] =
			sqrtf(fftout[dev->bins[j]][0] * fftout[dev->bins[j]][0] + fftout[dev->bins[j]][1] * fftout[dev->bins[j]][1]);
# ifdef NFM
			if(dev->channels[j].modulation == MOD_NFM) {
				dev->channels[j].complex_samples[2*dev->waveend] = fftout[dev->bins[j]][0];
				dev->channels[j].complex_samples[2*dev->waveend+1] = fftout[dev->bins[j]][1];
			}
# endif // NFM
		}
#endif // USE_BCM_VC

		dev->waveend += FFT_BATCH;

		if (dev->waveend >= WAVE_BATCH + AGC_EXTRA) {
			if (foreground) {
				GOTOXY(0, device_num * 17 + dev->row + 3);
			}
			for (int i = 0; i < dev->channel_count; i++) {
				AFC afc(dev, i);
				channel_t* channel = dev->channels + i;
#if defined (__arm__) || defined (__aarch64__)
				float agcmin2 = channel->agcmin * 4.5f;
				for (int j = 0; j < WAVE_BATCH + AGC_EXTRA; j++) {
					channel->waveref[j] = min(channel->wavein[j], agcmin2);
				}
#else
				__m128 agccap = _mm_set1_ps(channel->agcmin * 4.5f);
				for (int j = 0; j < WAVE_BATCH + AGC_EXTRA; j += 4) {
					__m128 t = _mm_loadu_ps(channel->wavein + j);
					_mm_storeu_ps(channel->waveref + j, _mm_min_ps(t, agccap));
				}
#endif
				for (int j = AGC_EXTRA; j < WAVE_BATCH + AGC_EXTRA; j++) {
					// auto noise floor
					if (channel->sqlevel < 0 && j % 16 == 0) {
						channel->agcmin = channel->agcmin * 0.97f + min(channel->agcavgslow, channel->agcmin) * 0.03f + 0.0001f;
					}

					// average power
					channel->agcavgslow = channel->agcavgslow * 0.99f + channel->waveref[j] * 0.01f;

					float sqlevel = channel->sqlevel > 0 ? (float)channel->sqlevel : 3.0f * channel->agcmin;
					if (channel->agcsq > 0) {
						channel->agcsq = max(channel->agcsq - 1, 1);
						if (channel->agcsq == 1 && channel->agcavgslow > sqlevel) {
							channel->agcsq = -AGC_EXTRA * 2;
							channel->axcindicate = '*';
							if(channel->modulation == MOD_AM) {
							// fade in
								for (int k = j - AGC_EXTRA; k < j; k++) {
									if (channel->wavein[k] > sqlevel) {
										channel->agcavgfast = channel->agcavgfast * 0.9f + channel->wavein[k] * 0.1f;
									}
								}
							}
						}
					} else {
						if (channel->wavein[j] > sqlevel) {
							if(channel->modulation == MOD_AM)
								channel->agcavgfast = channel->agcavgfast * 0.995f + channel->wavein[j] * 0.005f;
							channel->agclow = 0;
						} else {
							channel->agclow++;
						}
						channel->agcsq = min(channel->agcsq + 1, -1);
						if (channel->agclow == AGC_EXTRA - 12) {
							channel->agcsq = AGC_EXTRA * 2;
							channel->axcindicate = ' ';
							if(channel->modulation == MOD_AM) {
								// fade out
								for (int k = j - AGC_EXTRA + 1; k < j; k++) {
									channel->waveout[k] = channel->waveout[k - 1] * 0.94f;
								}
							}
						}
					}
					if(channel->agcsq != -1) {
						channel->waveout[j] = 0;
					} else {
						if(channel->modulation == MOD_AM) {
							channel->waveout[j] = (channel->wavein[j - AGC_EXTRA] - channel->agcavgfast) / (channel->agcavgfast * 1.5f);
							if (abs(channel->waveout[j]) > 0.8f) {
								channel->waveout[j] *= 0.85f;
								channel->agcavgfast *= 1.15f;
							}
						}
#ifdef NFM
						else {	// NFM
							multiply(channel->complex_samples[2*(j - AGC_EXTRA)], channel->complex_samples[2*(j - AGC_EXTRA)+1],
// FIXME: use j instead of wavecnt?
							channel->timeref_cos[channel->wavecnt],
							channel->timeref_nsin[channel->wavecnt],
							&rotated_r,
							&rotated_j);
							if(fm_demod == FM_FAST_ATAN2) {
								channel->waveout[j] = polar_disc_fast(rotated_r, rotated_j, channel->pr, channel->pj);
							} else if(fm_demod == FM_QUADRI_DEMOD) {
								channel->waveout[j] = fm_quadri_demod(rotated_r, rotated_j, channel->pr, channel->pj);
							}
							channel->pr = rotated_r;
							channel->pj = rotated_j;
// de-emphasis IIR + DC blocking
							channel->agcavgfast = channel->agcavgfast * 0.995f + channel->waveout[j] * 0.005f;
							channel->waveout[j] -= channel->agcavgfast;
							channel->waveout[j] = channel->waveout[j] * (1.0f - channel->alpha) + channel->waveout[j-1] * channel->alpha;
						}
#endif // NFM
					}
#ifdef NFM
					if(channel->modulation == MOD_NFM)
						channel->wavecnt = (channel->wavecnt + 1) % WAVE_RATE;
#endif // NFM
				}
				memmove(channel->wavein, channel->wavein + WAVE_BATCH, (dev->waveend - WAVE_BATCH) * 4);
#ifdef NFM
				if(channel->modulation == MOD_NFM)
					memmove(channel->complex_samples, channel->complex_samples + 2 * WAVE_BATCH, (dev->waveend - WAVE_BATCH) * 4 * 2);
#endif

#ifdef USE_BCM_VC
				afc.finalize(dev, i, fft->out);
#else
				afc.finalize(dev, i, fftout);
#endif

				if (foreground) {
					if(dev->mode == R_SCAN)
						printf("%4.0f/%3.0f%c %7.3f", channel->agcavgslow, channel->agcmin, channel->axcindicate, (dev->channels[0].frequency / 1000000.0));
					else
						printf("%4.0f/%3.0f%c", channel->agcavgslow, channel->agcmin, channel->axcindicate);
					fflush(stdout);
				}
			}
			dev->waveavail = 1;
			dev->waveend -= WAVE_BATCH;
			if(DEBUG) {
				gettimeofday(&te, NULL);
				debug_bulk_print("waveavail %lu.%lu %lu\n", te.tv_sec, te.tv_usec, (te.tv_sec - ts.tv_sec) * 1000000UL + te.tv_usec - ts.tv_usec);
				ts.tv_sec = te.tv_sec;
				ts.tv_usec = te.tv_usec;
			}
			pthread_cond_signal(&mp3_cond);
			dev->row++;
			if (dev->row == 12) {
				dev->row = 0;
			}
		}

		dev->bufs += speed2 * FFT_BATCH;
		if (dev->bufs >= BUF_SIZE) dev->bufs -= BUF_SIZE;
		device_num = (device_num + 1) % device_count;
	}
}

void usage() {
	cout<<"Usage: rtl_airband [options] [-c <config_file_path>]\n\
\t-h\t\t\tDisplay this help text\n\
\t-f\t\t\tRun in foreground, display textual waterfalls\n";
#ifdef NFM
	cout<<"\t-Q\t\t\tUse quadri correlator for FM demodulation (default is atan2)\n";
#endif
#if DEBUG
	cout<<"\t-d <file>\t\tLog debugging information to <file> (default is "<<DEBUG_PATH<<")\n";
#endif
	cout<<"\t-c <config_file_path>\tUse non-default configuration file\n\t\t\t\t(default: "<<CFGFILE<<")\n\
\t-v\t\t\tDisplay version and exit\n";
	exit(EXIT_SUCCESS);
}

int main(int argc, char* argv[]) {
#pragma GCC diagnostic ignored "-Wwrite-strings"
	char *cfgfile = CFGFILE;
	char *pidfile = PIDFILE;
#pragma GCC diagnostic warning "-Wwrite-strings"
	int opt;
	char optstring[16] = "fhvc:";

#ifdef NFM
	strcat(optstring, "Q");
#endif
#if DEBUG
	strcat(optstring, "d:");
#endif

	while((opt = getopt(argc, argv, optstring)) != -1) {
		switch(opt) {
#ifdef NFM
			case 'Q':
				fm_demod = FM_QUADRI_DEMOD;
				break;
#endif
#if DEBUG
			case 'd':
				debug_path = strdup(optarg);
				break;
#endif
			case 'f':
				foreground = 1;
				break;
			case 'c':
				cfgfile = optarg;
				break;
			case 'v':
				cout<<"RTLSDR-Airband version "<<RTL_AIRBAND_VERSION<<"\n";
				exit(EXIT_SUCCESS);
			case 'h':
			default:
				usage();
				break;
		}
	}
#if DEBUG
	if(!debug_path) debug_path = strdup(DEBUG_PATH);
	init_debug(debug_path);
#endif
#if !defined (__arm__) && !defined (__aarch64__)
#define cpuid(func,ax,bx,cx,dx)\
	__asm__ __volatile__ ("cpuid":\
		"=a" (ax), "=b" (bx), "=c" (cx), "=d" (dx) : "a" (func));
	int a,b,c,d;
	cpuid(1,a,b,c,d);
	if((int)((d >> 25) & 0x1)) {
		/* NOOP */
	} else {
		printf("Unsupported CPU.\n");
		error();
	}
#endif /* !__arm__ */

	// read config
	try {
		Config config;
		config.readFile(cfgfile);
		Setting &root = config.getRoot();
		if(root.exists("syslog")) do_syslog = root["syslog"];
		if(root.exists("pidfile")) pidfile = strdup(root["pidfile"]);
		if(root.exists("rtlsdr_buffers")) rtlsdr_buffers = (int)(root["rtlsdr_buffers"]);
		if(rtlsdr_buffers < 1) {
			cerr<<"Configuration error: rtlsdr_buffers must be greater than 0\n";
			error();
		}
		if(root.exists("shout_metadata_delay")) shout_metadata_delay = (int)(root["shout_metadata_delay"]);
		if(shout_metadata_delay < 0 || shout_metadata_delay > 2*TAG_QUEUE_LEN) {
			cerr<<"Configuration error: shout_metadata_delay is out of allowed range (0-"<<2 * TAG_QUEUE_LEN<<")\n";
			error();
		}
		if(root.exists("localtime") && (bool)root["localtime"] == true)
			use_localtime = true;
#ifdef NFM
		if(root.exists("tau"))
			alpha = ((int)root["tau"] == 0 ? 0.0f : exp(-1.0f/(WAVE_RATE * 1e-6 * (int)root["tau"])));
#endif
		Setting &devs = config.lookup("devices");
		device_count = devs.getLength();
		if (device_count < 1) {
			cerr<<"Configuration error: no devices defined\n";
			error();
		}
		struct sigaction sigact, pipeact;

		memset(&sigact, 0, sizeof(sigact));
		memset(&pipeact, 0, sizeof(pipeact));
		pipeact.sa_handler = SIG_IGN;
		sigact.sa_handler = &sighandler;
		sigaction(SIGPIPE, &pipeact, NULL);
		sigaction(SIGHUP, &sigact, NULL);
		sigaction(SIGINT, &sigact, NULL);
		sigaction(SIGQUIT, &sigact, NULL);
		sigaction(SIGTERM, &sigact, NULL);

		uintptr_t tempptr = (uintptr_t)malloc(device_count * sizeof(device_t)+31);
		tempptr &= ~0x0F;
		devices = (device_t *)tempptr;
		shout_init();
		if(do_syslog) openlog("rtl_airband", LOG_PID, LOG_DAEMON);

		if(root.exists("mixers")) {
			Setting &mx = config.lookup("mixers");
			mixers = (mixer_t *)calloc(mx.getLength(), sizeof(struct mixer_t));
			if(!mixers) {
				cerr<<"Cannot allocate memory for mixers\n";
				error();
			}
			if((mixer_count = parse_mixers(mx)) > 0) {
				mixers = (mixer_t *)realloc(mixers, mixer_count * sizeof(struct mixer_t));
				if(!mixers) {
					cerr<<"Cannot allocate memory for mixers\n";
					error();
				}
			} else {
				free(mixers);
			}
		} else {
			mixer_count = 0;
		}

		int devs_enabled = parse_devices(devs);
		if (devs_enabled < 1) {
			cerr<<"Configuration error: no devices defined\n";
			error();
		}
		debug_print("mixer_count=%d\n", mixer_count);
		for(int z = 0; z < mixer_count; z++) {
			mixer_t *m = &mixers[z];
			debug_print("mixer[%d]: name=%s, input_count=%d, output_count=%d\n", z, m->name, m->input_count, m->channel.output_count);
		}
		int device_count2 = rtlsdr_get_device_count();
		if (device_count2 < devs_enabled) {
			cerr<<"Not enough devices ("<<devs_enabled<<" configured, "<<device_count2<<" detected)\n";
			error();
		}
		for(int i = 0; i < devs_enabled; i++) {
			device_t* dev = devices + i;
			if(dev->device >= device_count2) {
				cerr<<"Specified device id "<<(int)dev->device<<" is greater or equal than number of devices ("<<device_count2<<")\n";
				error();
			}
		}
		device_count = devs_enabled;
	} catch(FileIOException e) {
			cerr<<"Cannot read configuration file "<<cfgfile<<"\n";
			error();
	} catch(ParseException e) {
			cerr<<"Error while parsing configuration file "<<cfgfile<<" line "<<e.getLine()<<": "<<e.getError()<<"\n";
			error();
	} catch(SettingNotFoundException e) {
			cerr<<"Configuration error: mandatory parameter missing: "<<e.getPath()<<"\n";
			error();
	} catch(SettingTypeException e) {
			cerr<<"Configuration error: invalid parameter type: "<<e.getPath()<<"\n";
			error();
	} catch(ConfigException e) {
			cerr<<"Unhandled config exception\n";
			error();
	}

	if(!foreground) {
		int pid1, pid2;
		if((pid1 = fork()) == -1) {
			cerr<<"Cannot fork child process: "<<strerror(errno)<<"\n";
			error();
		}
		if(pid1) {
			waitpid(-1, NULL, 0);
			return(0);
		} else {
			if((pid2 = fork()) == -1) {
				cerr<<"Cannot fork child process: "<<strerror(errno)<<"\n";
				error();
			}
			if(pid2) {
				return(0);
			} else {
				int nullfd, dupfd;
				if((nullfd = open("/dev/null", O_RDWR)) == -1) {
					log(LOG_CRIT, "Cannot open /dev/null: %s\n", strerror(errno));
					error();
				}
				for(dupfd = 0; dupfd <= 2; dupfd++) {
					if(dup2(nullfd, dupfd) == -1) {
						log(LOG_CRIT, "dup2(): %s\n", strerror(errno));
						error();
					}
				}
				if(nullfd > 2) close(nullfd);
				FILE *f = fopen(pidfile, "w");
				if(f == NULL) {
					log(LOG_CRIT, "Cannot write pidfile: %s\n", strerror(errno));
					error();
				} else {
					fprintf(f, "%ld\n", (long)getpid());
					fclose(f);
				}
			}
		}
	}

	log(LOG_INFO, "RTLSDR-Airband version %s starting\n", RTL_AIRBAND_VERSION);
	for (int i = 0; i < mixer_count; i++) {
		if(mixers[i].enabled == false)
			continue;		// no inputs connected = no need to initialize output
		channel_t *channel = &mixers[i].channel;
		if(channel->need_mp3)
			channel->lame = airlame_init(mixers[i].channel.mode);
		for (int k = 0; k < channel->output_count; k++) {
			output_t *output = channel->outputs + k;
			if(output->type == O_ICECAST)
				shout_setup((icecast_data *)(output->data), channel->mode);
		}
	}
	for (int i = 0; i < device_count; i++) {
		device_t* dev = devices + i;
		if(pthread_mutex_init(&dev->buffer_lock, NULL) != 0) {
			cerr<<"Failed to initialize buffer mutex for device "<<i<<" - aborting\n";
			error();
		}
		for (int j = 0; j < dev->channel_count; j++) {
			channel_t* channel = dev->channels + j;

			if(channel->need_mp3)
				channel->lame = airlame_init(channel->mode);
			for (int k = 0; k < channel->output_count; k++) {
				output_t *output = channel->outputs + k;
				if(output->type == O_ICECAST)
					shout_setup((icecast_data *)(output->data), channel->mode);
			}
		}
		pthread_create(&dev->rtl_thread, NULL, &rtlsdr_exec, dev);
		if(dev->mode == R_SCAN) {
			if(pthread_mutex_init(&dev->tag_queue_lock, NULL) != 0) {
				cerr<<"Failed to initialize mutex - aborting\n";
				error();
			}
// FIXME: not needed when freq_count == 1?
			pthread_create(&dev->controller_thread, NULL, &controller_thread, dev);
		}
	}

	int timeout = 50;		// 5 seconds
	while (atomic_get(&device_opened) != device_count && timeout > 0) {
		SLEEP(100);
		timeout--;
	}
	if(atomic_get(&device_opened) != device_count) {
		cerr<<"Some devices failed to initialize - aborting\n";
		error();
	}
	if (foreground) {
		printf("\e[1;1H\e[2J");

		GOTOXY(0, 0);
		printf("                                                                               ");
		for (int i = 0; i < device_count; i++) {
			GOTOXY(0, i * 17 + 1);
			for (int j = 0; j < devices[i].channel_count; j++) {
				printf(" %7.3f  ", devices[i].channels[j].frequency / 1000000.0);
			}
			if (i != device_count - 1) {
				GOTOXY(0, i * 17 + 16);
				printf("-------------------------------------------------------------------------------");
			}
		}
	}
	THREAD thread2;
	pthread_create(&thread2, NULL, &icecast_check, NULL);
	THREAD thread3;
	pthread_create(&thread3, NULL, &output_thread, NULL);
	THREAD thread4;
	if(mixer_count > 0)
		pthread_create(&thread4, NULL, &mixer_thread, NULL);

	demodulate();

	log(LOG_INFO, "cleaning up\n");
	for (int i = 0; i < device_count; i++) {
		rtlsdr_cancel_async(devices[i].rtlsdr);
		pthread_join(devices[i].rtl_thread, NULL);
		if(devices[i].mode == R_SCAN)
			pthread_join(devices[i].controller_thread, NULL);
	}
	log(LOG_INFO, "rtlsdr threads closed\n");
	close_debug();
	return 0;
}
// vim: ts=4

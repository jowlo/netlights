#include <err.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <ncurses.h>
#include <asoundlib.h>
#include <sndfile.h>

#include <fftw3.h>

#include "udpsend.h"

#define POWER_RANGES 10 

//do we send data out?
bool net;
//console bars?
bool showbars = false;

struct holder {
	SNDFILE *infile;
	snd_pcm_t *inhandle;
  snd_pcm_hw_params_t *hw_params;
	snd_pcm_format_t format;
	unsigned int rate;

	SF_INFO ininfo;

	snd_pcm_t *alsa_handle;

	fftw_complex *output;
	fftw_plan plan;

	int height, width;

	unsigned int samples_count;
  unsigned int fftout_count;

	double *blackman_window;

	double *samples;
	double *windowed_samples;

  double *power_spectrum;
  double *logx;

	double *max;
};

static void prepare_fftw(struct holder *holder)
{
	unsigned int a;

  // init buffer for samples
	holder->samples = fftw_alloc_real(holder->samples_count);
	if (!holder->samples)
		errx(3, "cannot allocate input");

  // init buffer for windowed samples
	holder->windowed_samples = fftw_alloc_real(holder->samples_count);
	if (!holder->samples)
		errx(3, "cannot allocate window space");

	// init fft output buffer
	holder->output = fftw_alloc_complex(holder->fftout_count);
	if (!holder->output)
		errx(3, "cannot allocate output");
  
  holder->max = (double *)malloc(holder->fftout_count * sizeof(double));
  for(int i = 0; i<holder->fftout_count; i++) holder->max[i] = 0.000000000000001;//;// FLT_MIN;
//  // null all
//	for (a = 0; a < holder->samples_count; a++) {
//		holder->samples[a] = 0;
//		holder->windowed_samples[a] = 0;
//	}
//  for (int i = 0; i < holder->fftout_count; i++) {
//		holder->output[i] = 0;
//  }

	// calculate a blackman window if not already done
  holder->blackman_window = (double*)malloc(holder->samples_count * sizeof(double));
   for(int i = 0; i < holder->samples_count; i++)
       holder->blackman_window[i] = 0.53836 - 0.46164*cos( 2*M_PI * i / ( holder->samples_count-1) );

	holder->plan = fftw_plan_dft_r2c_1d(holder->samples_count, holder->windowed_samples, holder->output, 0);
	if (!holder->plan)
		errx(3, "plan not created");
}

static void destroy_fftw(struct holder *holder)
{
	fftw_destroy_plan(holder->plan);
	fftw_free(holder->output);
	fftw_free(holder->samples);
}

/* compute avg of all channels , write to holder->samples */
static void compute_avg(struct holder *holder, float *buf, unsigned int count)
{
	unsigned int channels = holder->ininfo.channels;
//	printf("\nchannels: %d\n", holder->ininfo.channels);
//	printf("\n ==== Calculating average of channels ==== \n");

	for (int i = 0; i < count; i++) {
		holder->samples[i] = 0;
		for (int ch = 0; ch < channels; ch++)
			holder->samples[i] += buf[i * channels + ch];
		holder->samples[i] /= channels;
    //printf("buf@%d+%d::%f + %f  = sample@%d::%f \n", i*channels, i*channels+1, buf[i * channels], buf[i * channels +1],  i, holder->samples[i]);
	}
}

static void compute_fftw(struct holder *holder)
{

	//printf("\n ==== Calculating blackman window over samples ==== \n");

  // apply window
  for(int i = 0; i < holder->samples_count; ++i){
    holder->windowed_samples[i] = holder->samples[i] * holder->blackman_window[i];
    //printf("windowed_sample@%d::%f \n", i, holder->samples[i]);
  }
        
  // do the actual fft
	// for reference:
  //      holder->plan = fftw_plan_dft_r2c_1d(holder->samples_count, holder->windowed_samples, holder->output, 0);
	fftw_execute(holder->plan);

  //printf("holder->fftout_count: %d\n", holder->fftout_count);

  //for(int i = 0; i < holder->fftout_count; i++) {
  //  printf("out@%3d::%f + %f*I\n", i, holder->output[i][0], holder->output[i][1]);
  //}

//	for (int i = 0; i < holder->samples_count / 2 + 1; i++) {
//		holder->samples[i] = cabs(holder->output[i]);
//		if (holder->samples[i] > holder->max)
//			holder->max = holder->samples[i];
//	}
}

static void show_graph(struct holder *holder)
{
  // log10 on x axis
//  for(int i = 1; i < holder->fftout_count; i++) {
//    holder->power_spectrum[i] = log10(holder->power_spectrum[i]);
//  }

  // log10 on y axis
  // this is - no joke - taken from xkcd forums
  // i do not know how it works, neither does the poster remember
  //
  // n = N/2
  // float logx[n] - output array after applying "vertical" log to power[i]
//  double logn=log((double)holder->fftout_count);
//  for(int i=0; i<holder->fftout_count-1; i++) {
//     double exponent=(i*logn)/holder->fftout_count;
//  
//     double idx=exp(exponent)-1;
//           
//     int k=(int)idx;
//     double alfa=idx-k;
//  
//     holder->logx[i]=(1-alfa)*holder->power_spectrum[k]+alfa*holder->power_spectrum[k+1];
//  }
//  holder->logx[holder->fftout_count-1]=holder->power_spectrum[holder->fftout_count-1];



  double sum = 0;

  char send[2+ POWER_RANGES*2];
  memset(&send[0], 0, sizeof(send));
  send[1] = 0x7e; // pwm command


  int j = (holder->fftout_count/2)/POWER_RANGES;
  for(int i = 0; i < holder->fftout_count/2; i++) {
    //sum += holder->power_spectrum[i]/j;
    
    if(holder->max[i/j] < holder->power_spectrum[i]) holder->max[i/j] = holder->power_spectrum[i];

    if(holder->power_spectrum[i]/holder->max[i/j] > sum){
      sum = holder->power_spectrum[i]/holder->max[i/j];
    } 

    if(i%j == 0){
      //sum = sqrt(sum);
      //sum /= 100;

      //values
      printf("%7d: %.7f\t", i/j, sum);
      
      //improvised bars
      if(showbars){
        printf("%3d%.*s%.*s", i/j, ((int)(sum*16)), "================", 16-((int)(sum*16)<16?((int)(sum*16)):16), "                ");
      }
      if(sum*1024 > 30){
        send[2+ (i/j)*2] = ((u_int)(sum*1024) >> 8) & 0xff;
        send[2+ (i/j)*2+1] = (u_int)(sum*1024) & 0xff ;
      }

      sum = 0;
    }
  }
  //if(showbars) printf("\n");
  printf("\n");
  printf("%s",send);
  //strcat(send, "\n");
  if(net){
    int ret = udp_send(sizeof(send), send);
    //printf("net return = %d", ret);
  }
}

static void write_snd(struct holder *holder, float const *samples,
		unsigned int count)
{
	snd_pcm_sframes_t frames;

	frames = snd_pcm_writei(holder->alsa_handle, samples, count);
	if (frames < 0)
		frames = snd_pcm_recover(holder->alsa_handle, frames, 0);
	if (frames < 0)
		errx(2, "snd_pcm_writei failed: %s", snd_strerror(frames));
}

void decode(struct holder *holder)
{
	unsigned int channels = holder->ininfo.channels;
      printf("channels::%d ", channels);
	int16_t *buf;//[channels * holder->samples_count];
	float *fbuf;//[channels * holder->samples_count];
  buf = malloc(holder->samples_count * snd_pcm_format_width(holder->format) / 8 * 2);
  fbuf = malloc(holder->samples_count*sizeof(float));

	int count, short_read;
/*
	do {
		count = sf_readf_float(holder->infile, buf,
				holder->samples_count );
      //printf("\nread frames count::%d \n", count);

  // display buffer
  //  for(int i = 0; i < holder->samples_count; i++)
  //    printf("buf%d::%f \n",i, buf[i]);
		
    if (count <= 0)
			break;

		// the last chunk? 
		short_read = count != holder->samples_count;
		if (!short_read) {


		  // buffer should be completely filled with samples
			compute_avg(holder, buf, count);

			// holder->samples should be filled with samples (avg over channels)

			compute_fftw(holder);

			show_graph(holder);
			//usleep(10000);
		}

		write_snd(holder, buf, count);
	} while (!short_read);
*/

  while(1) {
    count = snd_pcm_readi(holder->inhandle, buf, holder->samples_count);
    //printf("samples read: %d\n", count);
    // display s16_le buffer 
    //for(int i = 0; i < holder->samples_count; i++){
    //  printf("b%3d::%d \n",i, buf[i]);
    //}

    for(int i = 0; i < holder->samples_count; i++){
      holder->samples[i] = (float)buf[i]/32768.0;
    //  printf("bf%d::%g \n",i, holder->samples[i]);
    }

	  //compute_avg(holder, fbuf, count);
    
    // display samples 
    //for(int i = 0; i < holder->samples_count; i++)
    //  printf("s%d::%f \n",i, holder->samples[i]);

		compute_fftw(holder);
    
    // allocate power spectrum
    if(holder->power_spectrum == NULL) {
      holder->power_spectrum = (double *)malloc(holder->fftout_count * sizeof(double));
    }

    // fill power spectrum
    for(int i = 0; i < holder->fftout_count; i++) {
      if(holder->power_spectrum[i] < 0) holder->power_spectrum[i] = - holder->power_spectrum[i];
      holder->power_spectrum[i] = (holder->output[i][0] * holder->output[i][0]) + (holder->output[i][1] * holder->output[i][1]) / holder->samples_count;
    }
    
    // log on x axis
    //for(int i = 1; i < holder->fftout_count; i++) {
    //  holder->power_spectrum[i] = log(holder->power_spectrum[i]);
    //}
    
    // display power_spectrum
    //for(int i = 0; i < holder->fftout_count; i++)
    //printf("p%d::%f \n",i, holder->power_spectrum[i]);
	   
	   show_graph(holder);
  }
}

void open_io(struct holder *holder, const char *filename)
{
	int err;
	holder->format = SND_PCM_FORMAT_S16_LE;
	holder->rate = 8000;

  if ((err = snd_pcm_open (&holder->inhandle, filename, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
    fprintf (stderr, "cannot open audio device %s (%s)\n", 
             filename,
             snd_strerror (err));
    exit (1);
  }

  fprintf(stdout, "audio interface opened\n");
		   
  if ((err = snd_pcm_hw_params_malloc (&holder->hw_params)) < 0) {
    fprintf (stderr, "cannot allocate hardware parameter structure (%s)\n",
             snd_strerror (err));
    exit (1);
  }

  fprintf(stdout, "hw_params allocated\n");
				 
  if ((err = snd_pcm_hw_params_any (holder->inhandle, holder->hw_params)) < 0) {
    fprintf (stderr, "cannot initialize hardware parameter structure (%s)\n",
             snd_strerror (err));
    exit (1);
  }

  fprintf(stdout, "hw_params initialized\n");
	
  if ((err = snd_pcm_hw_params_set_access (holder->inhandle, holder->hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
    fprintf (stderr, "cannot set access type (%s)\n",
             snd_strerror (err));
    exit (1);
  }

  fprintf(stdout, "hw_params access setted\n");
	
  if ((err = snd_pcm_hw_params_set_format (holder->inhandle, holder->hw_params, holder->format)) < 0) {
    fprintf (stderr, "cannot set sample format (%s)\n",
             snd_strerror (err));
    exit (1);
  }

  fprintf(stdout, "hw_params format setted\n");
	
  if ((err = snd_pcm_hw_params_set_rate_near (holder->inhandle, holder->hw_params, &holder->rate, 0)) < 0) {
    fprintf (stderr, "cannot set sample rate (%s)\n",
             snd_strerror (err));
    exit (1);
  }
	
  fprintf(stdout, "hw_params rate setted\n");

  if ((err = snd_pcm_hw_params_set_channels(holder->inhandle, holder->hw_params, 2)) < 0) {
    fprintf (stderr, "cannot set channel count (%s)\n",
             snd_strerror (err));
    exit (1);
  }

  fprintf(stdout, "hw_params channels setted\n");
	
  if ((err = snd_pcm_hw_params(holder->inhandle, holder->hw_params)) < 0) {
    fprintf (stderr, "cannot set parameters (%s)\n",
             snd_strerror (err));
    exit (1);
  }

/*
	if (!strcmp(filename, "-"))
		holder->infile = sf_open_fd(STDIN_FILENO, SFM_READ, &holder->ininfo, 1);
  else{

	  //holder->infile = sf_open(filename, SFM_READ, &holder->ininfo);
	  snd_pcm_open(&holder->inhandle, filename, SND_PCM_STREAM_CAPTURE, 0);
	  snd_pcm_prepare(&holder->inhandle);
  }

//	if (holder->infile == NULL)
//		errx(1, "open in: %s", sf_strerror(NULL));


	err = snd_pcm_open(&holder->alsa_handle, "default",
			SND_PCM_STREAM_PLAYBACK, 0);
	if (err < 0)
		errx(1, "alsa open: %s", snd_strerror(err));

	err = snd_pcm_set_params(holder->alsa_handle, SND_PCM_FORMAT_FLOAT,
			SND_PCM_ACCESS_RW_INTERLEAVED, holder->ininfo.channels,
			holder->ininfo.samplerate, 1, 500000);
	if (err < 0)
		errx(1, "alsa set_params: %s", snd_strerror(err));

*/
    holder->ininfo.channels = 1;
    holder->ininfo.samplerate = holder->rate;
}

void close_io(struct holder *holder)
{
	endwin();
	snd_pcm_close(holder->inhandle);
//	sf_close(holder->infile);
}

int main(int argc, char **argv)
{
	struct holder holder = {};

	if (argc < 2)
		errx(1, "bad arguments");

	open_io(&holder, argv[1]);
	
	printf("samplerate: %d\n", holder.ininfo.samplerate);

	holder.samples_count = holder.ininfo.samplerate / 100;
  holder.fftout_count = holder.samples_count/2 +1;

	printf("samples_count: %d\n", holder.samples_count);
	printf("fftout_count: %d\n", holder.fftout_count);

  if(argc >3){
	  udp_setup(argv[2], argv[3]);
	  net = true;
  }
  if(argc >4 && strcmp(argv[4], "-v") == 0) showbars = true;
    
	prepare_fftw(&holder);

	decode(&holder);

	destroy_fftw(&holder);

	close_io(&holder);

	return 0;
}

// hpguppi_atasnap_voltage_thread.c
//
// A Hashpipe thread that proceeses "voltage mode" packets sent by the ATA SNAP
// design from an input buffer (populated by hpguppi_ibverbs_pkt_thread) and
// assembles them into GUPPI RAW blocks.

// TODO TEST Wait for first (second?) start-of-block when transitioning into
//           LISTEN state so that the first block will be complete.
// TODO Add PSPKTS and PSDRPS status buffer fields for pktsock
// TODO TEST Set NETSTAE to idle in IDLE state
// TODO TEST IP_DROP_MEMBERSHIP needs mcast IP address (i.e. not 0.0.0.0)

#define _GNU_SOURCE 1
//#include <stdio.h>
//#include <sys/types.h>
#include <stdlib.h>
#include <sched.h>
#include <math.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "hashpipe.h"
#include "hpguppi_databuf.h"
#include "hpguppi_time.h"
#include "hpguppi_util.h"
#include "hpguppi_atasnap.h"
#include "hpguppi_ibverbs_pkt_thread.h"

// Change to 1 to use temporal memset() rather than non-temporal bzero_nt()
#if 0
#define bzero_nt(d,l) memset(d,0,l)
#endif

// Change to 1 to use temporal memcpy() rather than non-temporal memcpy_nt()
#if 0
#define memcpy_nt(dst,src,len) memcpy(dst,src,len)
#endif

#define ELAPSED_NS(start,stop) \
  (((int64_t)stop.tv_sec-start.tv_sec)*1000*1000*1000+(stop.tv_nsec-start.tv_nsec))

// Define run states.  Currently three run states are defined: IDLE, LISTEN,
// and RECORD.
//
// In the LISTEN and RECORD states, the PKTIDX field is updated with the value
// from received packets.  Whenever the first PKTIDX of a block is received
// (i.e. whenever PKTIDX is a multiple of pktidx_per_block), the value
// for PKTSTART and DWELL are read from the status buffer.  PKTSTART is rounded
// down, if needed, to ensure that it is a multiple of pktidx_per_block,
// then PKTSTART is written back to the status buffer.  DWELL is interpreted as
// the number of seconds to record and is used to calculate PKTSTOP (which gets
// rounded down, if needed, to be a multiple of pktidx_per_block).
//
// The IDLE state is entered when there is no DESTIP defined in the status
// buffer or it is 0.0.0.0.  In the IDLE state, the DESTIP value in the status
// buffer is checked once per second.  If it is found to be something other
// than 0.0.0.0, the state transitions to the LISTEN state and the current
// blocks are reinitialized.
//
// To be operationally compatible with other hpguppi net threads, a "command
// FIFO" is created and read from in all states, but commands sent there are
// ignored.  State transitions are controlled entirely by DESTIP and
// PKTSTART/DWELL status buffer fields.
//
// In the LISTEN state, incoming packets are processed (i.e. stored in the net
// thread's output buffer) and full blocks are passed to the next thread.  When
// the processed PKTIDX is equal to PKTSTART the state transitions to RECORD
// and the following actions occur:
//
//   1. The MJD of the observation start time is calculated from PKTIDX,
//      SYNCTIME, and other parameters.
//
//   2. The packet stats counters are reset
//
//   3. The STT_IMDJ and STT_SMJD are updated in the status buffer
//
//   4. STTVALID is set to 1
//
// In the RECORD state, incoming packets are processed (i.e. stored in the net
// thread's output buffer) and full blocks are passed to the next thread (same
// as in the LISTEN state).  When the processed PKTIDX is greater than or equal
// to PKTSTOP the state transitions to LISTEN and STTVALID is set to 0.
//
// The PKTSTART/PKTSTOP tests are done every time the work blocks are advanced.
//
// The downstream thread (i.e. hpguppi_rawdisk_thread) is expected to use a
// combination of PKTIDX, PKTSTART, PKTSTOP, and (optionally) STTVALID to
// determine whether the blocks should be discarded or processed (e.g. written
// to disk).

enum run_states {IDLE, LISTEN, RECORD};

// Structure related to block management
struct block_info {
  // Set at start of run
  struct hpguppi_input_databuf *dbin;  // Pointer to databuf from pkt thread
  struct hpguppi_input_databuf *dbout; // Pointer to overall shared mem databuf
  // Set at start of block
  int block_idx_out;                // Block index number in output databuf
  int64_t block_num;                // Absolute block number
  uint64_t pktidx_per_block;
  uint64_t pkts_per_block;
  // Incremented throughout duration of block
  uint32_t npacket;                 // Number of packets recevied so far
  // Fields set during block finalization
  uint32_t ndrop;                   // Count of expected packets not recevied
};

// Returns pointer to block_info's output data block
static char * block_info_data(const struct block_info *bi)
{
  return hpguppi_databuf_data(bi->dbout, bi->block_idx_out);
}

// Returns pointer to block_info's header
static char * block_info_header(const struct block_info *bi)
{
  return hpguppi_databuf_header(bi->dbout, bi->block_idx_out);
}

// Reset counter(s) in block_info
static void reset_block_info_stats(struct block_info *bi)
{
  bi->npacket=0;
  bi->ndrop=0;
}

// (Re-)initialize some or all fields of block_info bi.
// bi->dbout is set if dbout is non-NULL.
// bi->block_idx_out is set if block_idx_out >= 0.
// bi->block_num is always set and the stats are always reset.
// bi->pkts_per_block is set of pkt_size > 0.
static void init_block_info(struct block_info *bi,
    struct hpguppi_input_databuf *dbout, int block_idx_out, int64_t block_num,
    uint64_t pkts_per_block)
{
  if(dbout) {
    bi->dbout = dbout;
  }
  if(block_idx_out >= 0) {
    bi->block_idx_out = block_idx_out;
  }
  bi->block_num = block_num;
  if(pkts_per_block > 0) {
    bi->pkts_per_block = pkts_per_block;
  }
  reset_block_info_stats(bi);
}

// Update block's header info and set filled status (i.e. hand-off to downstream)
static void finalize_block(struct block_info *bi)
{
  if(bi->block_idx_out < 0) {
    hashpipe_error(__FUNCTION__, "block_info.block_idx_out == %d", bi->block_idx_out);
    pthread_exit(NULL);
  }
  char *header = block_info_header(bi);
  char dropstat[128];
  if(bi->pkts_per_block > bi->npacket) {
    bi->ndrop = bi->pkts_per_block - bi->npacket;
  }
#if 0
if(bi->pkts_per_block != bi->npacket) {
  printf("pktidx %ld pkperblk %ld npkt %u\n",
    bi->block_num * bi->pktidx_per_block,
    bi->pkts_per_block, bi->npacket);
}
#endif
  sprintf(dropstat, "%d/%lu", bi->ndrop, bi->pkts_per_block);
  hputi8(header, "PKTIDX", bi->block_num * bi->pktidx_per_block);
  hputi4(header, "NPKT", bi->npacket);
  hputi4(header, "NDROP", bi->ndrop);
  hputs(header, "DROPSTAT", dropstat);
  hpguppi_input_databuf_set_filled(bi->dbout, bi->block_idx_out);
}

// Advance to next block in data buffer.  This new block will contain
// absolute block block_num.
//
// NB: The caller must wait for the new data block to be free after this
// function returns!
static void increment_block(struct block_info *bi, int64_t block_num)
{
  if(bi->block_idx_out < 0) {
    hashpipe_warn(__FUNCTION__,
        "block_info.block_idx_out == %d", bi->block_idx_out);
  }
  if(bi->dbout->header.n_block < 1) {
    hashpipe_error(__FUNCTION__,
        "block_info.dbout->header.n_block == %d", bi->dbout->header.n_block);
    pthread_exit(NULL);
  }

  bi->block_idx_out = (bi->block_idx_out + 1) % bi->dbout->header.n_block;
  bi->block_num = block_num;
  reset_block_info_stats(bi);
}

// Wait for a block_info's databuf block to be free, then copy status buffer to
// block's header and clear block's data.  Calling thread will exit on error
// (should "never" happen).  Status buffer updates made after the copy to the
// block's header will not be seen in the block's header (e.g. by downstream
// threads).  Any status buffer fields that need to be updated for correct
// downstream processing of this block must be updated BEFORE calling this
// function.  Note that some of the block's header fields will be set when the
// block is finalized (see finalize_block() for details).
static void wait_for_block_free(const struct block_info * bi,
    hashpipe_status_t * st, const char * status_key)
{
  int rv;
  char netstat[80] = {0};
  char netbuf_status[80];
  int netbuf_full = hpguppi_input_databuf_total_status(bi->dbout);
  //struct timespec ts_sleep = {0, 10 * 1000 * 1000}; // 10 ms
  sprintf(netbuf_status, "%d/%d", netbuf_full, bi->dbout->header.n_block);

  hashpipe_status_lock_safe(st);
  {
    hgets(st->buf, status_key, sizeof(netstat), netstat);
    hputs(st->buf, status_key, "waitfree");
    hputs(st->buf, "NETBUFST", netbuf_status);
  }
  hashpipe_status_unlock_safe(st);

  while ((rv=hpguppi_input_databuf_wait_free(bi->dbout, bi->block_idx_out))
      != HASHPIPE_OK) {
    if (rv==HASHPIPE_TIMEOUT) {
      netbuf_full = hpguppi_input_databuf_total_status(bi->dbout);
      sprintf(netbuf_status, "%d/%d", netbuf_full, bi->dbout->header.n_block);
      hashpipe_status_lock_safe(st);
      hputs(st->buf, status_key, "outblocked");
      hputs(st->buf, "NETBUFST", netbuf_status);
      hashpipe_status_unlock_safe(st);
    } else {
      hashpipe_error("hpguppi_atasnap_voltage_thread",
          "error waiting for free databuf");
      pthread_exit(NULL);
    }
  }

  hashpipe_status_lock_safe(st);
  {
    hputs(st->buf, status_key, netstat);
    memcpy(block_info_header(bi), st->buf, HASHPIPE_STATUS_TOTAL_SIZE);
  }
  hashpipe_status_unlock_safe(st);

#if 0
  // TODO Move this out of net thread (takes too long)
  // TODO Just clear effective block size?
  //memset(block_info_data(bi), 0, BLOCK_DATA_SIZE);
  bzero_nt(block_info_data(bi), BLOCK_DATA_SIZE);
#else
  //nanosleep(&ts_sleep, NULL);
#endif
}

// The copy_packet_data_to_databuf() function does what it says: copies packet
// data into a data buffer.
//
// The data buffer block is identified by the block_info structure pointed to
// by the bi parameter.
//
// The p_oi parameter points to the observation's obs_info data.
//
// The p_fei parameter points to an ata_snap_feng_info structure containing the
// packet's metadata.
//
// The p_payload parameter points to the payload of the packet.
//
// This is for packets in [time (slowest), channel, pol (fastest)] order.  In
// other words:
//
//     T0C0P0 T0C0P1 T0C1P0 T0C1P1 ... T0CcP0 T0CcP1 <- t=0
//     T1C0P0 T1C0P1 T1C1P0 T1C1P1 ... T1CcP0 T1CcP1 <- t=1
//     ...
//     TtC0P0 TtC0P1 TtC1P0 TtC1P1 ... TtCcP0 TtCcP1 <- t=pkt_ntime-1
//
// GUPPI RAW block is ordered as:
//
//     t=0               t=1                   t=NTIME
//     F0T0C0P0 F0T0C0P1 F0T1C0P0 F0T1C0P1 ... F0TtC0P0 F0TtC0P1
//     F0T0C1P0 F0T0C1P1 F0T1C1P0 F0T1C1P1 ... F0TtC1P0 F0TtC1P1
//     ...
//     F0T0CcP0 F0T0CcP1 F0T1CcP0 F0T1CcP1 ... F0TtCcP0 F0TtCcP1
//     F1T0C0P0 F1T0C0P1 F1T1C0P0 F1T1C0P1 ... F1TtC0P0 F1TtC0P1
//     F1T0C1P0 F1T0C1P1 F1T1C1P0 F1T1C1P1 ... F1TtC1P0 F1TtC1P1
//     ...
//     ...
//     FfT0CcP0 FfT0CcP1 FfT1CcP0 FfT1CcP1 ... FfTtCcP0 FfTtCcP1
//
// where F is FID (f=NANTS-1), T is time (t=PKT_NTIME-1), C is channel
// (c=NSTRMS*PKT_NCHAN-1), and P is polarization.  Streams are not shown
// separately, which is why they are bundled in with channel number.  Each
// packet fills a 2D rectangle in the GUPPI RAW block, which can be shown
// somewhat pictorially like this (with time faster changing in the horizontal
// direction, then channel changing in the vertical direction) for a single
// PKTIDX value (i.e. this is a slice in time of the GUPPI RAW block):
//
//     [FID=0, STREAM=0, TIME=0:PKT_NTIME-1, CHAN=0:PKT_NCHAN-1] ...
//     [FID=0, STREAM=1, TIME=0:PKT_NTIME-1, CHAN=0:PKT_NCHAN-1] ...
//      ...
//     [FID=0, STREAM=s, TIME=0:PKT_NTIME-1, CHAN=0:PKT_NCHAN-1] ...
//     [FID=1, STREAM=0, TIME=0:PKT_NTIME-1, CHAN=0:PKT_NCHAN-1] ...
//     [FID=1, STREAM=1, TIME=0:PKT_NTIME-1, CHAN=0:PKT_NCHAN-1] ...
//      ...
//      ...
//     [FID=f, STREAM=s, TIME=0:PKT_NTIME-1, CHAN=0:PKT_NCHAN-1] ...
//

static void copy_packet_data_to_databuf(const struct block_info *bi,
    const struct ata_snap_obs_info * p_oi,
    const struct ata_snap_feng_info * p_fei,
    const uint8_t * p_payload)
{
  // We copy the two pols together as a uint16_t type
  const uint16_t * src = (uint16_t *)p_payload;
  uint16_t * dst_base = (uint16_t *)block_info_data(bi);
  uint16_t * dst;
  int t, c;

  // ostride is the spacing, in units of sizeof(uint16_t), from one channel to
  // the next for a given F engine, stream, and pktidx value.  It is equal to
  // NTIME == pktidx_per_block * pkt_ntime.
  const size_t ostride = bi->pktidx_per_block * p_oi->pkt_ntime;

  // stream_stride is the size of a single stream for a single F engine for all
  // NTIME samples of the block and all channels in a stream (i.e. in a packet):
  const int stream_stride = ATA_SNAP_PKT_SIZE_PAYLOAD * bi->pktidx_per_block;

  // fid_stride is the size of all streams of a single F engine:
  const int fid_stride = stream_stride * p_oi->nstrm;

  // pktidx_stride is the size of a single channel for a single PKTIDX value
  // (i.e. for a single packet):
  const int pktidx_stride = p_oi->pkt_nchan;

  // Stream is the "channel chunk" for this FID
  const int stream = (p_fei->feng_chan - p_oi->schan) / p_oi->pkt_nchan;

  // Advance dst_base to...
  dst_base += p_fei->feng_id * fid_stride // first location of this FID, then
           +  stream * stream_stride // first location of this stream, then
           +  (p_fei->pktidx - bi->pktidx_per_block) * pktidx_stride; // to this pktidx

#if 0
printf("feng_id       = %lu\n", p_fei->feng_id);
printf("nstrm         = %d\n", p_oi->nstrm);
printf("pkt_nchan     = %d\n", p_oi->pkt_nchan);
printf("feng_chan     = %lu\n", p_fei->feng_chan);
printf("schan         = %d\n", p_oi->schan);
printf("ostride       = 0x%08lx\n", ostride);
printf("stream_stride = %d\n", stream_stride);
printf("fid_stride    = %d\n", fid_stride);
printf("pktidx_stride = %d\n", pktidx_stride);
printf("stream        = %d\n", stream);
printf("dst           = 0x%p\n", dst);
#endif

  // Copy samples linearly from packet, strided to dst
  for(t=0; t < p_oi->pkt_ntime; t++) {
    dst = dst_base;
    for(c=0; c < p_oi->pkt_nchan; c++) {
      *dst = *src;
      dst += ostride;
      src++;
    }
    dst_base++;
  }
}

// Check the given pktidx value against the status buffer's PKTSTART/PKTSTOP
// values. Logic goes something like this:
//   if PKTSTART <= pktidx < PKTSTOPs
//     if STTVALID == 0
//       STTVALID=1
//       calculate and store STT_IMJD, STT_SMJD
//     endif
//     return RECORD
//   else
//     STTVALID=0
//     return LISTEN
//   endif
static
enum run_states check_start_stop(hashpipe_status_t *st, uint64_t pktidx)
{
  enum run_states retval = LISTEN;
  uint32_t sttvalid = 0;
  uint64_t pktstart = 0;
  uint64_t pktstop = 0;

  uint32_t pktntime = ATASNAP_DEFAULT_PKTNTIME;
  uint64_t synctime = 0;
  double chan_bw = 1.0;

  double realtime_secs = 0.0;
  struct timespec ts;

  int    stt_imjd = 0;
  int    stt_smjd = 0;
  double stt_offs = 0;

  hashpipe_status_lock_safe(st);
  {
    hgetu4(st->buf, "STTVALID", &sttvalid);
    hgetu8(st->buf, "PKTSTART", &pktstart);
    hgetu8(st->buf, "PKTSTOP", &pktstop);

    if(pktstart <= pktidx && pktidx < pktstop) {
      retval = RECORD;
      hputs(st->buf, "DAQSTATE", "RECORD");

      if(sttvalid != 1) {
        hputu4(st->buf, "STTVALID", 1);

        hgetu4(st->buf, "PKTNTIME", &pktntime);
        hgetr8(st->buf, "CHAN_BW", &chan_bw);
        hgetu8(st->buf, "SYNCTIME", &synctime);

        // Calc real-time seconds since SYNCTIME for pktidx:
        //
        //                      pktidx * pktntime
        //     realtime_secs = -------------------
        //                        1e6 * chan_bw
        if(chan_bw != 0.0) {
          realtime_secs = pktidx * pktntime / (1e6 * fabs(chan_bw));
        }

        ts.tv_sec = (time_t)(synctime + rint(realtime_secs));
        ts.tv_nsec = (long)((realtime_secs - rint(realtime_secs)) * 1e9);

        get_mjd_from_timespec(&ts, &stt_imjd, &stt_smjd, &stt_offs);

        hputu4(st->buf, "STT_IMJD", stt_imjd);
        hputu4(st->buf, "STT_SMJD", stt_smjd);
        hputr8(st->buf, "STT_OFFS", stt_offs);
      }
    } else {
      hputs(st->buf, "DAQSTATE", "LISTEN");
      if(sttvalid != 0) {
        hputu4(st->buf, "STTVALID", 0);
      }
    }
  }
  hashpipe_status_unlock_safe(st);

  return retval;
}

// This thread's init() function, if provided, is called by the Hashpipe
// framework at startup to allow the thread to perform initialization tasks
// such as setting up network connections or GPU devices.
static int init(hashpipe_thread_args_t *args)
{
  // Local aliases to shorten access to args fields
  // Our input buffer happens to be a hpguppi_input_databuf
  hpguppi_input_databuf_t *dbin  = (hpguppi_input_databuf_t *)args->ibuf;
  const char * thread_name = args->thread_desc->name;
  const char * status_key = args->thread_desc->skey;
  hashpipe_status_t *st = &args->st;

  // Non-network essential paramaters
  int blocsize=BLOCK_DATA_SIZE;
  int directio=1;
  int nbits=4;
  int npol=4;
  double obsfreq=0;
  double chan_bw=900.0/4096;
  double obsbw=256*chan_bw;
  int obsnchan=1;
  int nants=1;
  int overlap=0;
  double tbin=1e-6;
  char obs_mode[80] = {0};
  struct rlimit rlim;

  strcpy(obs_mode, "RAW");

  // Verify that the IBVPKTSZ was specified as expected/requried
  if(hpguppi_pktbuf_slot_offset(dbin, ATA_SNAP_PKT_OFFSET_HEADER) %
      PKT_ALIGNMENT_SIZE != 0
  || hpguppi_pktbuf_slot_offset(dbin, ATA_SNAP_PKT_OFFSET_PAYLOAD) %
      PKT_ALIGNMENT_SIZE != 0) {
    errno = EINVAL;
    hashpipe_error(thread_name, "IBVPKTSZ!=%d,%d,[...]",
        ATA_SNAP_PKT_OFFSET_HEADER, ATA_SNAP_PKT_SIZE_HEADER);
    return HASHPIPE_ERR_PARAM;
  }

  // Set RLIMIT_RTPRIO to 1
  getrlimit(RLIMIT_RTPRIO, &rlim);
  if (rlim.rlim_max >= 1){
    rlim.rlim_cur = 1;
    if(setrlimit(RLIMIT_RTPRIO, &rlim)) {
      hashpipe_error(thread_name, "setrlimit(RLIMIT_RTPRIO)");
    }
  }
  else{
    hashpipe_info(thread_name, "Not setting rlim_cur=1 because rlim_max = %d < 1.", rlim.rlim_max);
  }

  struct sched_param sched_param = {
    .sched_priority = 1
  };
  if(sched_setscheduler(0, SCHED_RR, &sched_param)) {
    hashpipe_error(thread_name, "sched_setscheduler");
  }

  hashpipe_status_lock_safe(st);
  {
    // Get info from status buffer if present (no change if not present)
    hgeti4(st->buf, "BLOCSIZE", &blocsize);
    hgeti4(st->buf, "DIRECTIO", &directio);
    hgeti4(st->buf, "NANTS", &nants);
    hgeti4(st->buf, "NBITS", &nbits);
    hgeti4(st->buf, "NPOL", &npol);
    hgetr8(st->buf, "OBSFREQ", &obsfreq);
    hgetr8(st->buf, "OBSBW", &obsbw);
    hgetr8(st->buf, "CHAN_BW", &chan_bw);
    hgeti4(st->buf, "OBSNCHAN", &obsnchan);
    hgeti4(st->buf, "OVERLAP", &overlap);
    hgets(st->buf, "OBS_MODE", sizeof(obs_mode), obs_mode);

    // Prevent div-by-zero errors (should never happen...)
    if(nants == 0) {
      nants = 1;
      hputi4(st->buf, "NANTS", nants);
    }

    // If CHAN_BW is zero, set to default value (1 MHz)
    if(chan_bw == 0.0) {
      chan_bw = 1.0;
    }

    // Calculate tbin and obsbw from chan_bw
    tbin = 1e-6 / fabs(chan_bw);
    obsbw = chan_bw * obsnchan / nants;

    // Update status buffer (in case fields were not there before).
    hputs(st->buf, "DAQSTATE", "LISTEN");
    hputi4(st->buf, "BLOCSIZE", blocsize);
    hputi4(st->buf, "DIRECTIO", directio);
    hputi4(st->buf, "NBITS", nbits);
    hputi4(st->buf, "NPOL", npol);
    hputr8(st->buf, "OBSBW", obsbw);
    hputr8(st->buf, "CHAN_BW", chan_bw);
    hputi4(st->buf, "OBSNCHAN", obsnchan);
    hputi4(st->buf, "OVERLAP", overlap);
    hputs(st->buf, "PKTFMT", "ATASNAPV");
    hputr8(st->buf, "TBIN", tbin);
    hputs(st->buf, "OBS_MODE", obs_mode);
    hputi4(st->buf, "NDROP", 0);
    // Set status_key to init
    hputs(st->buf, status_key, "init");
  }
  hashpipe_status_unlock_safe(st);

  // Success!
  return 0;
}

static void * run(hashpipe_thread_args_t * args)
{
#if 0
int debug_i=0, debug_j=0;
#endif
  // Local aliases to shorten access to args fields
  // Our input and output buffers happen to be a hpguppi_input_databuf
  hpguppi_input_databuf_t *dbin  = (hpguppi_input_databuf_t *)args->ibuf;
  hpguppi_input_databuf_t *dbout = (hpguppi_input_databuf_t *)args->obuf;
  hashpipe_status_t *st = &args->st;
  const char * thread_name = args->thread_desc->name;
  const char * status_key = args->thread_desc->skey;

  // String version of destination address
  char dest_ip_stream_str[80] = {};
  char dest_ip_stream_str_new[80] = {};
  char * pchar;
  // Numeric form of dest_ip
  struct in_addr dest_ip;
  int dest_idx;
  // Number of destination IPs we are listening for
  int nstreams = 0;
  // Max flows allowed (optionally from hpguppi_ibvpkt_thread via status
  // buffer)
  uint32_t max_flows = 16;
  // Port to listen on
  uint32_t port = 4015;

  // Current run state
  //enum run_states state = LISTEN;
  unsigned waiting = 0;
  // Update status_key with idle state and get max_flows, port
  hashpipe_status_lock_safe(st);
  {
    hputs(st->buf, status_key, "listen");
    hgetu4(st->buf, "MAXFLOWS", &max_flows);
    hgetu4(st->buf, "BINDPORT", &port);
    // Store bind port in status buffer (in case it was not there before).
    hputu4(st->buf, "BINDPORT", port);
  }
  hashpipe_status_unlock_safe(st);

  // Make sure we got a non-zero max_flows
  if(max_flows == 0) {
    hashpipe_error(thread_name, "MAXFLOWS must be non-zero!");
    return NULL;
  }

  // Misc counters, etc
  int rv=0;
  int i;

#if 0
  uint64_t u64;
  uint8_t u8 = 0;
  uint8_t *pu8in = (uint8_t *)dbin;
  uint8_t *pu8out = (uint8_t *)dbout;
  for(u64=0; u64<sizeof(hpguppi_input_databuf_t); u64+=4096) {
    if(u8 || !u8) {
      u8 += pu8in[u64];
      u8 += pu8out[u64];
    }
  }
  hashpipe_info(thread_name, "db pagein sum is %u", u8);
#endif
  memset(dbout->block, 0, sizeof(dbout->block));
  hashpipe_info(thread_name,
      "set %lu bytes in dbout to 0", sizeof(dbout->block));

  for(i=0; i<N_INPUT_BLOCKS; i++) {
    hashpipe_info(thread_name, "db_in  block %2d : %p %p", i,
        hpguppi_databuf_data(dbin, i),
        hpguppi_databuf_data(dbin, i) + BLOCK_DATA_SIZE - 1);
  }

  for(i=0; i<N_INPUT_BLOCKS; i++) {
    hashpipe_info(thread_name, "db_out block %2d : %p %p", i,
        hpguppi_databuf_data(dbout, i),
        hpguppi_databuf_data(dbout, i) + BLOCK_DATA_SIZE - 1);
  }

  // The incoming packets are taken from blocks of the input databuf and then
  // converted to GUPPI RAW format in blocks of the output databuf to pass to
  // the downstream thread.  We currently support two active output blocks (aka
  // "working blocks").  Working blocks are associated with absolute output
  // block numbers, which are simply PKTIDX values divided by the number of
  // packets per block (discarding any remainder).  Let the block numbers for
  // the first working block (wblk[0]) be W.  The block number for the second
  // working block (wblk[1]) will be W+1.  Incoming packets corresponding to
  // block W or W+1 are placed in the corresponding data buffer block.
  // Incoming packets for block W+2 cause block W to be "finalized" and handed
  // off to the downstream thread, working block 1 moves to working block 0 and
  // working block 1 is incremented to be W+2.  Things get "interesting" when a
  // packet is recevied for block < W or block > W+2.  Packets for block W-1
  // are ignored.  Packets with PKTIDX P corresponding block < W-1 or block >
  // W+2 cause the current working blocks' block numbers to be reset such that
  // W will refer to the block containing P and W+1 will refer to the block
  // after that.
  //
  // wblk is a two element array of block_info structures (i.e. the working
  // blocks)
  struct block_info wblk[2];
  int wblk_idx;

  // Packet block variables
  uint64_t pkt_seq_num = 0;
  int64_t pkt_blk_num = 0; // Signed to avoid problems comparing with -1
  uint64_t start_seq_num=0;
  uint64_t stop_seq_num=0;
  uint64_t status_seq_num;
  //uint64_t last_seq_num=2048;
  //uint64_t nextblock_seq_num=0;
  uint64_t dwell_blocks = 0;
  double dwell_seconds = 300.0;
  double chan_bw = 1.0;
  double tbin = 1.0e-6;

  // Heartbeat variables
  time_t lasttime = 0;
  time_t curtime = 0;
  char timestr[32] = {0};

  // Variables for working with the input databuf
  struct hpguppi_pktbuf_info * pktbuf_info = hpguppi_pktbuf_info_ptr(dbin);
  int block_idx_in = 0;
  const int npkts_per_block_in = pktbuf_info->slots_per_block;
  const int slot_size = pktbuf_info->slot_size;
  struct timespec timeout_in = {0, 50 * 1000 * 1000}; // 50 ms

  // Variables for counting packets and bytes.
  uint64_t packet_count = 0; // Counts packets between updates to status buffer
  uint64_t u64tmp = 0; // Used for status buffer interactions
  //uint64_t max_recvpkt_count = 0;
  uint64_t ndrop_total = 0;
  uint64_t nlate = 0;

  // Variables for handing received packets
  uint8_t * p_u8pkt;
  struct ata_snap_ibv_pkt * p_pkt = NULL;
  const uint8_t * p_payload = NULL;

  // Structure to hold observation info, init all fields to invalid values
  struct ata_snap_obs_info obs_info;
  ata_snap_obs_info_init(&obs_info);

  // OBSNCHAN is total number of channels handled by this instance.
  // For arrays like ATA, it is NANTS*NSTRM*PKTNCHAN.
  int obsnchan = 1;
  // PKTIDX per block (depends on obs_info, specifically nants).  Init to 0 to
  // cause div-by-zero error if using it unintialized (crash early, crash
  // hard!).
  uint32_t pktidx_per_block = 0;
  // Effective block size (will be less than BLOCK_DATA_SIZE when
  // BLOCK_DATA_SIZE is not divisible by NANTS, PKTNCHAN and/or PKTNTIME.
  // Historically, BLOCSIZE gets stored as a signed 4 byte integer
  int32_t eff_block_size;

  // Structure to hold feng info from packet
  struct ata_snap_feng_info feng_info = {0};

  // Variables for tracking timing stats
  //
  // ts_start_recv(N) to ts_stop_recv(N) is the time spent in the "receive" call.
  // ts_stop_recv(N) to ts_start_recv(N+1) is the time spent processing received data.
  struct timespec ts_start_recv = {0}, ts_stop_recv = {0};
  struct timespec ts_prev_phys = {0}, ts_curr_phys = {0};
  struct timespec ts_input_full0 = {0};
  struct timespec ts_free_input = {0};

  // We compute NETGBPS every block as (bits_processed_net / ns_processed_net)
  // We compute NETPKPS every block as (1e9 * pkts_processed_net / ns_processed_net)
  float netgbps = 0.0, netpkps = 0.0;
  uint64_t bits_processed_net = 0;
  uint64_t pkts_processed_net = 0;
  uint64_t ns_processed_net = 0;

  // We compute PHYSGBPS every second as (bits_processed_phys / ns_processed_phys)
  // We compute PHYSPKPS every second as (1e9 * pkts_processed_phys / ns_processed_phys)
  float physgbps = 0.0, physpkps = 0.0;
  uint64_t bits_processed_phys = 0;
  uint64_t pkts_processed_phys = 0;
  uint64_t ns_processed_phys = 0;

  // Used to calculate moving average of fill-to-free times for input blocks
  uint64_t fill_to_free_elapsed_ns;
  uint64_t fill_to_free_moving_sum_ns = 0;
  uint64_t fill_to_free_block_ns[N_INPUT_BLOCKS] = {0};

  //struct timespec ts_sleep = {0, 10 * 1000 * 1000}; // 10 ms

#if 0
  // Allocate a 2K buffer into which packet will be non-temporally copied
  // before processing.  This buffer will be cached (due to parsing of the
  // headers), but the input databuf blocks will not be cached.
  if((rv = posix_memalign((void **)&p_spdpkt, 4096, MAX_PKT_SIZE))) {
    errno = rv;
    hashpipe_error(thread_name, "cannot allocate page aligned packet buffer");
    return NULL;
  }
#endif

  // Initialize working blocks
  for(wblk_idx=0; wblk_idx<2; wblk_idx++) {
    init_block_info(wblk+wblk_idx, dbout, wblk_idx, wblk_idx, 0);
    wait_for_block_free(wblk+wblk_idx, st, status_key);
  }

  // Get any obs info from status buffer, store values
  hashpipe_status_lock_safe(st);
  {
    // Read (no change if not present)
    hgetu4(st->buf, "FENCHAN",  &obs_info.fenchan);
    hgetu4(st->buf, "NANTS",    &obs_info.nants);
    hgetu4(st->buf, "NSTRM",    &obs_info.nstrm);
    hgetu4(st->buf, "PKTNTIME", &obs_info.pkt_ntime);
    hgetu4(st->buf, "PKTNCHAN", &obs_info.pkt_nchan);
    hgeti4(st->buf, "SCHAN",    &obs_info.schan);

    // If obs_info is valid
    if(ata_snap_obs_info_valid(obs_info)) {
      // Update obsnchan, pktidx_per_block, and eff_block_size
      obsnchan = ata_snap_obsnchan(obs_info);
      pktidx_per_block = ata_snap_pktidx_per_block(BLOCK_DATA_SIZE, obs_info);
      eff_block_size = ata_snap_block_size(BLOCK_DATA_SIZE, obs_info);

      hputs(st->buf, "OBSINFO", "VALID");
    } else {
      hputs(st->buf, "OBSINFO", "INVALID");
    }

    // Write (store default/invlid values if not present)
    hputu4(st->buf, "FENCHAN",  obs_info.fenchan);
    hputu4(st->buf, "NANTS",    obs_info.nants);
    hputu4(st->buf, "NSTRM",    obs_info.nstrm);
    hputu4(st->buf, "PKTNTIME", obs_info.pkt_ntime);
    hputu4(st->buf, "PKTNCHAN", obs_info.pkt_nchan);
    hputi4(st->buf, "SCHAN",    obs_info.schan);

    hputu4(st->buf, "OBSNCHAN", obsnchan);
    hputu4(st->buf, "PIPERBLK", pktidx_per_block);
    hputi4(st->buf, "BLOCSIZE", eff_block_size);
  }
  hashpipe_status_unlock_safe(st);

  // Wait for ibvpkt thread to be running, then it's OK to add/remove flows.
  hpguppi_ibvpkt_wait_running(st);

  // Main loop
  while (run_threads()) {

    // Mark ts_stop_recv as unset
    ts_stop_recv.tv_sec = 0;

    // Wait for data
    do {
      clock_gettime(CLOCK_MONOTONIC, &ts_start_recv);
      // If ts_stop_recv has been set
      if(ts_stop_recv.tv_sec != 0) {
        // Accumulate processing time
        ns_processed_net += ELAPSED_NS(ts_stop_recv, ts_start_recv);
      }
      rv = hpguppi_input_databuf_wait_filled_timeout(
          dbin, block_idx_in, &timeout_in);
      clock_gettime(CLOCK_MONOTONIC, &ts_stop_recv);

      time(&curtime);

      if(rv && curtime == lasttime) {
        // No, continue receiving
        continue;
      }

      // Got packets or new second

      // We perform some status buffer updates every second
      if(curtime != lasttime) {
        lasttime = curtime;
        ctime_r(&curtime, timestr);
        timestr[strlen(timestr)-1] = '\0'; // Chop off trailing newline

        // Update PHYSGBPS and PHYSPKPS
        clock_gettime(CLOCK_MONOTONIC, &ts_curr_phys);
        if(ts_prev_phys.tv_sec != 0) {
          ns_processed_phys = ELAPSED_NS(ts_prev_phys, ts_curr_phys);
          physgbps = ((float)bits_processed_phys) / ns_processed_phys;
          physpkps = (1e9 * pkts_processed_phys) / ns_processed_phys;
          bits_processed_phys = 0;
          pkts_processed_phys = 0;
        }
        ts_prev_phys = ts_curr_phys;

        hashpipe_status_lock_safe(st);
        {
          hputs(st->buf, "DAQPULSE", timestr);

          hgetu8(st->buf, "NPKTS", &u64tmp);
          u64tmp += packet_count; packet_count = 0;
          hputu8(st->buf, "NPKTS", u64tmp);

          hputr4(st->buf, "PHYSGBPS", physgbps);
          hputr4(st->buf, "PHYSPKPS", physpkps);

          // Update obs_info
          //
          // Read (no change if not present)
          hgetu4(st->buf, "FENCHAN",  &obs_info.fenchan);
          hgetu4(st->buf, "NANTS",    &obs_info.nants);
          hgetu4(st->buf, "NSTRM",    &obs_info.nstrm);
          hgetu4(st->buf, "PKTNTIME", &obs_info.pkt_ntime);
          hgetu4(st->buf, "PKTNCHAN", &obs_info.pkt_nchan);
          hgeti4(st->buf, "SCHAN",    &obs_info.schan);

          // If obs_info is valid
          if(ata_snap_obs_info_valid(obs_info)) {
            // Update obsnchan, pktidx_per_block, and eff_block_size
            obsnchan = ata_snap_obsnchan(obs_info);
            pktidx_per_block = ata_snap_pktidx_per_block(BLOCK_DATA_SIZE, obs_info);
            eff_block_size = ata_snap_block_size(BLOCK_DATA_SIZE, obs_info);

            hputu4(st->buf, "OBSNCHAN", obsnchan);
            hputu4(st->buf, "PIPERBLK", pktidx_per_block);
            hputi4(st->buf, "BLOCSIZE", eff_block_size);

            hputs(st->buf, "OBSINFO", "VALID");
          } else {
            hputs(st->buf, "OBSINFO", "INVALID");
          }
          //
          // End update obs_info

          // Get DESTIP address
          hgets(st->buf,  "DESTIP",
              sizeof(dest_ip_stream_str_new), dest_ip_stream_str_new);
        }
        hashpipe_status_unlock_safe(st);

        // If DESTIP has changed
        if(strcmp(dest_ip_stream_str, dest_ip_stream_str_new)) {

          // Make sure the change is allowed
          // If we are listening, the only allowed change is to "0.0.0.0"
          if(nstreams > 0 && strcmp(dest_ip_stream_str_new, "0.0.0.0")) {
            hashpipe_error(thread_name,
                "already listening to %s, can't switch to %s",
                dest_ip_stream_str, dest_ip_stream_str_new);
          } else {
            // Parse the A.B.C.D+N notation
            //
            // Nul terminate at '+', if present
            if((pchar = strchr(dest_ip_stream_str_new, '+'))) {
              // Null terminate dest_ip portion and point to N
              *pchar = '\0';
            }

            // If the IP address fails to satisfy aton()
            if(!inet_aton(dest_ip_stream_str_new, &dest_ip)) {
              hashpipe_error(thread_name, "invalid DESTIP: %s", dest_ip_stream_str_new);
            } else {
              // If switching to "0.0.0.0"
              if(dest_ip.s_addr == INADDR_ANY) {
                // Remove all flows
                hashpipe_info(thread_name, "dest_ip %s (removing %d flows)\nDESTIP 0.0.0.0 is not applicable.",
                    dest_ip_stream_str_new, nstreams);
                for(dest_idx=0; dest_idx < nstreams; dest_idx++) {
                  if(hpguppi_ibvpkt_flow(dbin, dest_idx, IBV_FLOW_SPEC_UDP,
                        0, 0, 0, 0, 0, 0, 0, 0))
                  {
                    hashpipe_error(thread_name, "hashpipe_ibv_flow error");
                  }
                }
                nstreams = 0;
                // TODO Update the IDLE/CAPTURE state???
              } else {
                // Get number of streams
                nstreams = 1;
                if(pchar) {
                  nstreams = strtoul(pchar+1, NULL, 0);
                  nstreams++;
                }
                if(nstreams > max_flows) {
                  nstreams = max_flows;
                }
                // Add flows for stream
                hashpipe_info(thread_name, "dest_ip %s+%s flows",
                    dest_ip_stream_str_new, pchar ? pchar+1 : "0");
                hashpipe_info(thread_name, "adding %d flows", nstreams);
                for(dest_idx=0; dest_idx < nstreams; dest_idx++) {
                  if(hpguppi_ibvpkt_flow(dbin, dest_idx, IBV_FLOW_SPEC_UDP,
                        //hibv_ctx->mac, NULL, 0, 0,
                        NULL, NULL, 0, 0,
                        0, ntohl(dest_ip.s_addr)+dest_idx, 0, port))
                  {
                    hashpipe_error(thread_name, "hashpipe_ibv_flow error");
                    break;
                  }
                }
                // TODO Update the IDLE/CAPTURE state???
              } // end zero/non-zero IP

              // Restore '+' if it was found
              if(pchar) {
                *pchar = '+';
              }
              // Save the new DESTIP string
              strncpy(dest_ip_stream_str, dest_ip_stream_str_new,
                  sizeof(dest_ip_stream_str));
            } // end ip valid
          } // end destip change allowed

          // Store (possibly unchanged) DESTIP/NSTRM
          hashpipe_status_lock_safe(st);
          {
            hputs(st->buf,  "DESTIP", dest_ip_stream_str);
            hputu4(st->buf, "NSTRM", nstreams);
          }
          hashpipe_status_unlock_safe(st);
        } // end destip changed
      } // curtime != lasttime

      // Set status field to "waiting" if we are not getting packets
      if(rv && run_threads() && !waiting) {
        hashpipe_status_lock_safe(st);
        {
          hputs(st->buf, status_key, "waiting");
        }
        hashpipe_status_unlock_safe(st);
        waiting=1;
      }

      // Will exit if thread has been cancelled
      pthread_testcancel();
    } while (rv && run_threads()); // end wait for data loop

    if(!run_threads()) {
      // We're outta here!
      // But first mark the block free if we got one.
      if(!rv) {
        hpguppi_input_databuf_set_free(dbin, block_idx_in);
        clock_gettime(CLOCK_MONOTONIC, &ts_free_input);
        fprintf(stderr, "final fill-to-free %ld ns\n", ELAPSED_NS(ts_stop_recv, ts_free_input));
      }
      break;
    }

    // If obs_info is invalid
    if(!ata_snap_obs_info_valid(obs_info)) {
      hashpipe_status_lock_safe(st);
      {
        hputs(st->buf, status_key, "obsinfo");
      }
      hashpipe_status_unlock_safe(st);
      waiting=0;

      // Mark input block free
      hpguppi_input_databuf_set_free(dbin, block_idx_in);
      // Advance to next input block
      block_idx_in = (block_idx_in + 1) % dbin->header.n_block;

      // Go back to waiting for block to be filled
      continue;
    }
    hashpipe_info(thread_name, "Got packets!");

    // Got packet(s)!  Update status if needed.
    if (waiting) {
      hashpipe_status_lock_safe(st);
      {
        hputs(st->buf, status_key, "receiving");
      }
      hashpipe_status_unlock_safe(st);
      waiting=0;
    }

    if(ts_input_full0.tv_sec == 0) {
      ts_input_full0 = ts_stop_recv;
    }

    // For each packet: process all packets
    p_u8pkt = (uint8_t *)hpguppi_databuf_data(dbin, block_idx_in);
    for(i=0; i < npkts_per_block_in; i++, p_u8pkt += slot_size) {
#if 0
      // Non-temporally copy packet into cached buffer
      memcpy_nt(p_spdpkt, p_u8pkt, MAX_PKT_SIZE);
#else
      p_pkt = (struct ata_snap_ibv_pkt *)p_u8pkt;
#endif

      // TODO Validate that this is a valid packet for us!
#if 0
for(debug_i=0; debug_i<8; debug_i++) {
  printf("%04x:", 16*debug_i);
  for(debug_j=0; debug_j<16; debug_j++) {
    printf(" %02x", ((uint8_t *)p_spdpkt)[16*debug_i+debug_j]);
  }
  printf("\n");
}
printf("\n");
fflush(stdout);
#endif

      // Parse packet
      p_payload = ata_snap_parse_ibv_packet(p_pkt, &feng_info);

#if 0
      // Warn about unexpected payload sizes, and ignore
      if(feng_info.payload_size != 1024) {
        hashpipe_warn(thread_name, "unexpected payload size %u",
            feng_info.payload_size);
#if 0
        for(i=0; i<144; i++) {
          if(i%16 == 0) fprintf(stderr, "%04x:", i);
          fprintf(stderr, " %02x", p_u8pkt[i]);
          if(i%16 == 15) fprintf(stderr, "\n");
        }
#endif
        continue;
      }
#endif

      // Ignore packets with FID >= NANTS
      if(feng_info.feng_id >= obs_info.nants) {
        continue;
      }

      // Count packet and the payload bits
      packet_count++;
      pkts_processed_net++;
      pkts_processed_phys++;
      bits_processed_net += 8 * ATA_SNAP_PKT_SIZE_PAYLOAD;
      bits_processed_phys += 8 * ATA_SNAP_PKT_SIZE_PAYLOAD;

      // Get packet index and absolute block number for packet
      pkt_seq_num = feng_info.pktidx;
      pkt_blk_num = pkt_seq_num / pktidx_per_block;

#if 0
if(i==0) {
fprintf(stderr, "pkt_seq_num = %lu\n", pkt_seq_num);
fprintf(stderr, "pkt_blk_num = %lu\n", pkt_blk_num);
fprintf(stderr, "pktidx       = 0x%016lx\n", feng_info.pktidx   );
fprintf(stderr, "feng_id      = 0x%016lx\n", feng_info.feng_id  );
fprintf(stderr, "feng_chan    = 0x%016lx\n", feng_info.feng_chan);
}
#endif

      // We update the status buffer at the start of each block
      // Also read PKTSTART, DWELL to calculate start/stop seq numbers.
      if(pkt_seq_num % pktidx_per_block == 0
          && pkt_seq_num != status_seq_num) {
        status_seq_num  = pkt_seq_num;

        // Update NETGBPS and NETPKPS
        if(ns_processed_net != 0) {
          netgbps = ((float)bits_processed_net) / ns_processed_net;
          netpkps = (1e9 * pkts_processed_net) / ns_processed_net;
          bits_processed_net = 0;
          pkts_processed_net = 0;
          ns_processed_net = 0;
        }

        hashpipe_status_lock_safe(st);
        {
          hputi8(st->buf, "PKTIDX", pkt_seq_num);
          hputi4(st->buf, "BLOCSIZE", eff_block_size);

          hgetu8(st->buf, "PKTSTART", &start_seq_num);
          start_seq_num -= start_seq_num % pktidx_per_block;
          hputu8(st->buf, "PKTSTART", start_seq_num);

          hgetr8(st->buf, "DWELL", &dwell_seconds);
          hputr8(st->buf, "DWELL", dwell_seconds); // In case it wasn't there

          hputr4(st->buf, "NETGBPS", netgbps);
          hputr4(st->buf, "NETPKPS", netpkps);

          // Get CHAN_BW and calculate/store TBIN
          hgetr8(st->buf, "CHAN_BW", &chan_bw);
          // If CHAN_BW is zero, set to default value (1 MHz)
          if(chan_bw == 0.0) {
            chan_bw = 1.0;
          }
          tbin = 1e-6 / fabs(chan_bw);
          hputr8(st->buf, "TBIN", tbin);

          // Dwell blocks is equal to:
          //
          //       dwell_seconds
          //     ------------------
          //     tbin * ntime/block
          //
          // To get an integer number of blocks, simply truncate
          dwell_blocks = trunc(dwell_seconds / (tbin * ata_snap_pkt_per_block(BLOCK_DATA_SIZE, obs_info)));

          stop_seq_num = start_seq_num + pktidx_per_block * dwell_blocks;
          hputi8(st->buf, "PKTSTOP", stop_seq_num);

          hgetu8(st->buf, "NDROP", &u64tmp);
          u64tmp += ndrop_total; ndrop_total = 0;
          hputu8(st->buf, "NDROP", u64tmp);

          hgetu8(st->buf, "NLATE", &u64tmp);
          u64tmp += nlate; nlate = 0;
          hputu8(st->buf, "NLATE", u64tmp);
        }
        hashpipe_status_unlock_safe(st);
      } // End status buffer block update

      // Manage blocks based on pkt_blk_num
      if(pkt_blk_num == wblk[1].block_num + 1) {
        // Time to advance the blocks!!!
#if 0
printf("next block (%ld == %ld + 1)\n", pkt_blk_num, wblk[1].block_num);
#endif

        // Finalize first working block
        finalize_block(wblk);
        // Update ndrop counter
        ndrop_total += wblk->ndrop;
        // Shift working blocks
        wblk[0] = wblk[1];
        // Check start/stop using wblk[0]'s first PKTIDX
        check_start_stop(st, wblk[0].block_num * pktidx_per_block);
        // Increment last working block
        increment_block(&wblk[1], pkt_blk_num);
        // Wait for new databuf data block to be free
        wait_for_block_free(&wblk[1], st, status_key);
      }
      // Check for PKTIDX discontinuity
      else if(pkt_blk_num < wblk[0].block_num - 1
      || pkt_blk_num > wblk[1].block_num + 1) {
#if 0
printf("reset blocks (%ld <> [%ld - 1, %ld + 1])\n", pkt_blk_num, wblk[0].block_num, wblk[1].block_num);
#endif
        // Should only happen when transitioning into LISTEN, so warn about it
        hashpipe_warn(thread_name,
            "working blocks reinit due to packet discontinuity (PKTIDX %lu)",
            pkt_seq_num);

#ifdef USE_WORKER_THREADS
        wait_for_job_completion(pjq);
#endif // USE_WORKER_THREADS

        // Re-init working blocks for block number *after* current packet's block
        // and clear their data buffers
        for(wblk_idx=0; wblk_idx<2; wblk_idx++) {
          init_block_info(wblk+wblk_idx, NULL, -1, pkt_blk_num+wblk_idx+1,
              eff_block_size / ATA_SNAP_PKT_SIZE_PAYLOAD);
#if 0
          // Clear data buffer
          // TODO Move this out of net thread (takes too long)
          //memset(block_info_data(wblk+wblk_idx), 0, eff_block_size);
          bzero_nt(block_info_data(wblk+wblk_idx), eff_block_size);
#else
          //nanosleep(&ts_sleep, NULL);
#endif
        }

        // Check start/stop using wblk[0]'s first PKTIDX
        check_start_stop(st, wblk[0].block_num * pktidx_per_block);
// This happens after discontinuities (e.g. on startup), so don't warn about
// it.
      } else if(pkt_blk_num == wblk[0].block_num - 1) {
        // Ignore late packet, continue on to next one
        // TODO Move this check above the "once per block" status buffer
        // update (so we don't accidentally update status buffer based on a
        // late packet)?
        nlate++;
#if 0
        // Should "never" happen, so warn about it
        hashpipe_warn(thread_name,
            "ignoring late packet (PKTIDX %lu)",
            pkt_seq_num);
#endif
      }

#if 0
printf("packet block: %ld   working blocks: %ld %lu\n", pkt_blk_num, wblk[0].block_num, wblk[1].block_num);
#endif

      // TODO Check START/STOP status???

      // Once we get here, compute the index of the working block corresponding
      // to this packet.  The computed index may not correspond to a valid
      // working block!
      wblk_idx = pkt_blk_num - wblk[0].block_num;

      // Only copy packet data and count packet if its wblk_idx is valid
      if(0 <= wblk_idx && wblk_idx < 2) {
        // Update block's packets per block.  Not needed for each packet, but
        // probably just as fast to do it for each packet rather than
        // check-and-update-only-if-needed for each packet.
        wblk[wblk_idx].pkts_per_block = eff_block_size / ATA_SNAP_PKT_SIZE_PAYLOAD;
        wblk[wblk_idx].pktidx_per_block = pktidx_per_block;

        // Copy packet data to data buffer of working block
        copy_packet_data_to_databuf(wblk+wblk_idx,
            &obs_info, &feng_info, p_payload);

        // Count packet for block and for processing stats
        wblk[wblk_idx].npacket++;
      }

    } // end for each packet

    // Mark input block free
    hpguppi_input_databuf_set_free(dbin, block_idx_in);

    // Update moving sum (for moving average)
    clock_gettime(CLOCK_MONOTONIC, &ts_free_input);
    fill_to_free_elapsed_ns = ELAPSED_NS(ts_stop_recv, ts_free_input);
    // Add new value, subtract old value
    fill_to_free_moving_sum_ns +=
        fill_to_free_elapsed_ns - fill_to_free_block_ns[block_idx_in];
    // Store new value
    fill_to_free_block_ns[block_idx_in] = fill_to_free_elapsed_ns;

    if(block_idx_in == N_INPUT_BLOCKS - 1) {
      hashpipe_status_lock_safe(st);
      {
        hputr8(st->buf, "NETBLKMS",
            round((double)fill_to_free_moving_sum_ns / N_INPUT_BLOCKS) / 1e6);
      }
      hashpipe_status_unlock_safe(st);
    }

#if 0
    fprintf(stderr, "blkin %d fill at %ld free +%ld ns (%d packets)\n",
        block_idx_in,
        ELAPSED_NS(ts_input_full0, ts_stop_recv),
        ELAPSED_NS(ts_stop_recv, ts_free_input), njobs);
#endif

    // Advance to next input block
    block_idx_in = (block_idx_in + 1) % dbin->header.n_block;

    // Will exit if thread has been cancelled
    pthread_testcancel();
  } // end main loop

  hashpipe_info(thread_name, "exiting!");
  pthread_exit(NULL);

  return NULL;
}

static hashpipe_thread_desc_t thread_desc = {
    name: "hpguppi_atasnap_voltage_thread",
    skey: "NETSTAT",
    init: init,
    run:  run,
    ibuf_desc: {hpguppi_input_databuf_create},
    obuf_desc: {hpguppi_input_databuf_create}
};

static __attribute__((constructor)) void ctor()
{
  register_hashpipe_thread(&thread_desc);
}

// vi: set ts=2 sw=2 et :

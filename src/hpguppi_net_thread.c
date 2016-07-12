/* hpguppi_net_thread.c
 *
 * Routine to read packets from network and put them
 * into shared memory blocks.
 */

#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>

#include "hashpipe.h"

#include "hpguppi_databuf.h"
#include "hpguppi_params.h"
#include "hpguppi_udp.h"
#include "hpguppi_time.h"

#define PKTSOCK_BYTES_PER_FRAME (16384)
#define PKTSOCK_FRAMES_PER_BLOCK (8)
#define PKTSOCK_NBLOCKS (800)
#define PKTSOCK_NFRAMES (PKTSOCK_FRAMES_PER_BLOCK * PKTSOCK_NBLOCKS)

/* It's easier to just make these global ... */
static unsigned long long npacket_total=0, ndropped_total=0, nbogus_total=0;

/* Structs/functions to more easily deal with multiple 
 * active blocks being filled
 */
struct datablock_stats {
    struct hpguppi_input_databuf *db; // Pointer to overall shared mem databuf
    int block_idx;                    // Block index number in databuf
    unsigned long long packet_idx;    // Index of first packet number in block
    size_t packet_data_size;          // Data size of each packet
    int packets_per_block;            // Total number of packets to go in the block
    int overlap_packets;              // Overlap between blocks in packets
    int npacket;                      // Number of packets filled so far
    int ndropped;                     // Number of dropped packets so far
    unsigned long long last_pkt;      // Last packet seq number written to block
};

#if 0
// Defined in guppi_net_thread_codd.c

/* get the thread specific pid */
pid_t gettid();

/* Update block header info, set filled status */
void finalize_block(struct datablock_stats *d);

/* Push all blocks down a level, losing the first one */
void block_stack_push(struct datablock_stats *d, int nblock);

/* Go to next block in set */
void increment_block(struct datablock_stats *d, 
        unsigned long long next_seq_num);

/* Check whether a certain seq num belongs in the data block */
int block_packet_check(struct datablock_stats *d, 
        unsigned long long seq_num);
#endif // 0

/* Reset all counters */
void reset_stats(struct datablock_stats *d) {
    d->npacket=0;
    d->ndropped=0;
    d->last_pkt=0;
}

/* Reset block params */
void reset_block(struct datablock_stats *d) {
    d->block_idx = -1;
    d->packet_idx = 0;
    reset_stats(d);
}

/* Initialize block struct */
void init_block(struct datablock_stats *d, struct hpguppi_input_databuf *db, 
        size_t packet_data_size, int packets_per_block, int overlap_packets) {
    d->db = db;
    d->packet_data_size = packet_data_size;
    d->packets_per_block = packets_per_block;
    d->overlap_packets = overlap_packets;
    reset_block(d);
}

/* Update block header info, set filled status */
void finalize_block(struct datablock_stats *d) {
    char *header = hpguppi_databuf_header(d->db, d->block_idx);
    hputi4(header, "PKTIDX", d->packet_idx);
    hputi4(header, "PKTSIZE", d->packet_data_size);
    hputi4(header, "NPKT", d->npacket);
    hputi4(header, "NDROP", d->ndropped);
    hpguppi_input_databuf_set_filled(d->db, d->block_idx);
}

/* Push all blocks down a level, losing the first one */
void block_stack_push(struct datablock_stats *d, int nblock) {
    int i;
    for (i=1; i<nblock; i++) 
        memcpy(&d[i-1], &d[i], sizeof(struct datablock_stats));
}

/* Go to next block in set */
void increment_block(struct datablock_stats *d, 
        unsigned long long next_seq_num) {
    d->block_idx = (d->block_idx + 1) % d->db->header.n_block;
    d->packet_idx = next_seq_num - (next_seq_num 
            % (d->packets_per_block - d->overlap_packets));
    reset_stats(d);
    // TODO: wait for block free here?
}

/* Check whether a certain seq num belongs in the data block */
int block_packet_check(struct datablock_stats *d, 
        unsigned long long seq_num) {
    if (seq_num < d->packet_idx) return(-1);
    else if (seq_num >= d->packet_idx + d->packets_per_block) return(1);
    else return(0);
}

/* Return packet index from a pktsock frame that is assumed to contain a UDP
 * packet.
 */
unsigned long long hpguppi_pktsock_seq_num(const unsigned char *p_frame) {
    // XXX Temp for new baseband mode, blank out top 8 bits which 
    // contain channel info.
    unsigned long long tmp = be64toh(*(uint64_t *)PKT_UDP_DATA(p_frame));
    tmp &= 0x00FFFFFFFFFFFFFF;
    return tmp ;
}

/* Write a search mode (filterbank) style packet (from pktsock) into the
 * datablock.  Also zeroes out any dropped packets.
 */
void write_search_packet_to_block_from_pktsock_frame(
        struct datablock_stats *d, unsigned char *p_frame) {
    const unsigned long long seq_num = hpguppi_pktsock_seq_num(p_frame);
    int next_pos = seq_num - d->packet_idx;
    int cur_pos=0;
    if (d->last_pkt > d->packet_idx) cur_pos = d->last_pkt - d->packet_idx + 1;
    char *dataptr = hpguppi_databuf_data(d->db, d->block_idx)
        + cur_pos*d->packet_data_size;
    for (; cur_pos<next_pos; cur_pos++) {
        memset(dataptr, 0, d->packet_data_size);
        dataptr += d->packet_data_size;
        d->npacket++;
        d->ndropped++;
    }
    hpguppi_udp_packet_data_copy_from_payload(dataptr,
        (char *)PKT_UDP_DATA(p_frame), (size_t)PKT_UDP_SIZE(p_frame));
    d->last_pkt = seq_num;
    //d->packet_idx++; // XXX I think this is wrong..
    d->npacket++;
}

/* Write a baseband mode packet into the block.  Includes a 
 * corner-turn (aka transpose) of dimension nchan.
 */
void write_baseband_packet_to_block_from_pktsock_frame(
        struct datablock_stats *d, unsigned char *p_frame, int nchan) {

    const unsigned long long seq_num = hpguppi_pktsock_seq_num(p_frame);
    int block_pkt_idx = seq_num - d->packet_idx;
    hpguppi_udp_packet_data_copy_transpose_from_payload(
            hpguppi_databuf_data(d->db, d->block_idx),
            nchan, block_pkt_idx, d->packets_per_block,
            (char *)PKT_UDP_DATA(p_frame),
            (size_t)PKT_UDP_SIZE(p_frame));

    /* Consider any skipped packets to have been dropped,
     * update counters.
     */
    if (d->last_pkt < d->packet_idx) d->last_pkt = d->packet_idx;

    if (seq_num == d->last_pkt) {
        d->npacket++;
    } else {
        d->npacket += seq_num - d->last_pkt;
        d->ndropped += seq_num - d->last_pkt - 1;
    }

    d->last_pkt = seq_num;
}

static int init(hashpipe_thread_args_t *args)
{
    /* Read network params */
    char bindhost[80];
    int bindport = 60000;

    strcpy(bindhost, "eth4");

    hashpipe_status_t st = args->st;

    hashpipe_status_lock_safe(&st);
    // Get info from status buffer if present (no change if not present)
    hgets(st.buf, "BINDHOST", 80, bindhost);
    hgeti4(st.buf, "BINDPORT", &bindport);
    // Store bind host/port info etc in status buffer
    hputs(st.buf, "BINDHOST", bindhost);
    hputi4(st.buf, "BINDPORT", bindport);
    hashpipe_status_unlock_safe(&st);

    /* Set up pktsock */
    struct hashpipe_pktsock *p_ps = (struct hashpipe_pktsock *)
        malloc(sizeof(struct hashpipe_pktsock));

    if(!p_ps) {
        perror(__FUNCTION__);
        return -1;
    }

    // Make frame_size be a divisor of block size so that frames will be
    // contiguous in mapped mempory.  block_size must also be a multiple of
    // page_size.  Easiest way is to oversize the frames to be 16384 bytes, which
    // is bigger than we need, but keeps things easy.
    p_ps->frame_size = PKTSOCK_BYTES_PER_FRAME;
    // total number of frames
    p_ps->nframes = PKTSOCK_NFRAMES;
    // number of blocks
    p_ps->nblocks = PKTSOCK_NBLOCKS;

    int rv = hashpipe_pktsock_open(p_ps, bindhost, PACKET_RX_RING);
    if (rv!=HASHPIPE_OK) {
        hashpipe_error("hpguppi_net_thread", "Error opening pktsock.");
        pthread_exit(NULL);
    }

    // Store packet socket pointer in args
    args->user_data = p_ps;

    // Success!
    return 0;
}

static void *run(hashpipe_thread_args_t * args)
{
    // Local aliases to shorten access to args fields
    // Our output buffer happens to be a hpguppi_input_databuf
    hpguppi_input_databuf_t *db = (hpguppi_input_databuf_t *)args->obuf;
    hashpipe_status_t st = args->st;
    const char * status_key = args->thread_desc->skey;

    /* Read in general parameters */
    struct hpguppi_params gp;
    struct psrfits pf;
    pf.sub.dat_freqs = NULL;
    pf.sub.dat_weights = NULL;
    pf.sub.dat_offsets = NULL;
    pf.sub.dat_scales = NULL;
    char status_buf[HASHPIPE_STATUS_TOTAL_SIZE];
    hashpipe_status_lock_safe(&st);
    memcpy(status_buf, st.buf, HASHPIPE_STATUS_TOTAL_SIZE);
    hashpipe_status_unlock_safe(&st);
    hpguppi_read_obs_params(status_buf, &gp, &pf);
    pthread_cleanup_push((void *)hpguppi_free_psrfits, &pf);

    /* Read network params */
    struct hpguppi_pktsock_params ps_params;
    hpguppi_read_pktsock_params(status_buf, &ps_params);

    /* Time parameters */
    int stt_imjd=0, stt_smjd=0;
    double stt_offs=0.0;

    /* See which packet format to use */
    int use_parkes_packets=0, baseband_packets=1;
    int nchan=0, npol=0, acclen=0;
    nchan = pf.hdr.nchan;
    npol = pf.hdr.npol;
    if (strncmp(ps_params.packet_format, "PARKES", 6)==0) { use_parkes_packets=1; }
    if (use_parkes_packets) {
        printf("hpguppi_net_thread: Using Parkes UDP packet format.\n");
        acclen = gp.decimation_factor;
        if (acclen==0) { 
            hashpipe_error("hpguppi_net_thread", 
                    "ACC_LEN must be set to use Parkes format");
            pthread_exit(NULL);
        }
    }

    /* Figure out size of data in each packet, number of packets
     * per block, etc.  Changing packet size during an obs is not
     * recommended.
     */
    int block_size;
    size_t packet_data_size = hpguppi_udp_packet_datasize(ps_params.packet_size);
    if (use_parkes_packets) 
        packet_data_size = parkes_udp_packet_datasize(ps_params.packet_size);
    unsigned packets_per_block; 
    if (hgeti4(status_buf, "BLOCSIZE", &block_size)==0) {
            block_size = db->header.block_size;
            hputi4(status_buf, "BLOCSIZE", block_size);
    } else {
        if (block_size > db->header.block_size) {
            hashpipe_error("hpguppi_net_thread", "BLOCSIZE > databuf block_size");
            block_size = db->header.block_size;
            hputi4(status_buf, "BLOCSIZE", block_size);
        }
    }
    packets_per_block = block_size / packet_data_size;

    /* If we're in baseband mode, figure out how much to overlap
     * the data blocks.
     */
    int overlap_packets=0;
    if (baseband_packets) {
        if (hgeti4(status_buf, "OVERLAP", &overlap_packets)==0) {
            overlap_packets = 0; // Default to no overlap
        } else {
            // XXX This is only true for 8-bit, 2-pol data:
            int samples_per_packet = packet_data_size / nchan / (size_t)4;
            if (overlap_packets % samples_per_packet) {
                hashpipe_error("hpguppi_net_thread", 
                        "Overlap is not an integer number of packets");
                overlap_packets = (overlap_packets/samples_per_packet+1);
                hputi4(status_buf, "OVERLAP", 
                        overlap_packets*samples_per_packet);
            } else {
                overlap_packets = overlap_packets/samples_per_packet;
            }
        }
    }

    /* List of databuf blocks currently in use */
    unsigned i;
    const int nblock = 2;
    struct datablock_stats blocks[nblock];
    for (i=0; i<nblock; i++) 
        init_block(&blocks[i], db, packet_data_size, packets_per_block, 
                overlap_packets);

    /* Convenience names for first/last blocks in set */
    struct datablock_stats *fblock, *lblock;
    fblock = &blocks[0];
    lblock = &blocks[nblock-1];

    /* Misc counters, etc */
    int rv;
    char *curdata=NULL, *curheader=NULL;
    unsigned long long seq_num, last_seq_num=2048, nextblock_seq_num=0;
    long long seq_num_diff;
    double drop_frac_avg=0.0;
    const double drop_lpf = 0.25;
    int netbuf_full = 0;
    char netbuf_status[128] = {};

    // Drop all packets to date
    unsigned char *p_frame;
    while((p_frame=hashpipe_pktsock_recv_frame_nonblock(&ps_params.ps))) {
        hashpipe_pktsock_release_frame(p_frame);
    }

    /* Main loop */
    unsigned force_new_block=0, waiting=-1;
    while (run_threads()) {

        /* Wait for data */
        do {
            p_frame = hashpipe_pktsock_recv_udp_frame(
                &ps_params.ps, ps_params.port, 1000); // 1 second timeout

            /* Set "waiting" flag */
            if (!p_frame && run_threads() && waiting!=1) {
                hashpipe_status_lock_safe(&st);
                hputs(st.buf, status_key, "waiting");
                hashpipe_status_unlock_safe(&st);
                waiting=1;
            }
        } while (!p_frame && run_threads());

        if(!run_threads()) {
            // We're outta here!
            hashpipe_pktsock_release_frame(p_frame);
            break;
        }

        /* Check packet size */
        if(ps_params.packet_size == 0) {
            ps_params.packet_size = PKT_UDP_SIZE(p_frame) - 8;
        } else if(ps_params.packet_size != PKT_UDP_SIZE(p_frame) - 8) {
            /* Unexpected packet size, ignore? */
            nbogus_total++;
            if(nbogus_total % 1000000 == 0) {
                hashpipe_status_lock_safe(&st);
                hputi4(st.buf, "NBOGUS", nbogus_total);
                hputi4(st.buf, "PKTSIZE", PKT_UDP_SIZE(p_frame)-8);
                hashpipe_status_unlock_safe(&st);
            }
            // Release frame!
            hashpipe_pktsock_release_frame(p_frame);
            continue; 
        }

        /* Update status if needed */
        if (waiting!=0) {
            hashpipe_status_lock_safe(&st);
            hputs(st.buf, status_key, "receiving");
            hashpipe_status_unlock_safe(&st);
            waiting=0;
        }

        /* Convert packet format if needed */
        if (use_parkes_packets) {
            parkes_to_guppi_from_payload(
                (char *)PKT_UDP_DATA(p_frame), acclen, npol, nchan);
        }

        /* Check seq num diff */
        seq_num = hpguppi_pktsock_seq_num(p_frame);
        seq_num_diff = seq_num - last_seq_num;
        if (seq_num_diff<=0) { 
            if (seq_num_diff<-1024) { force_new_block=1; }
            else if (seq_num_diff==0) {
                char msg[256];
                sprintf(msg, "Received duplicate packet (seq_num=%lld)", 
                        seq_num);
                hashpipe_warn("hpguppi_net_thread", msg);
            }
            else {
              // Release frame!
              hashpipe_pktsock_release_frame(p_frame);
              /* No going backwards */
              continue;
            }
        } else { 
            force_new_block=0; 
            npacket_total += seq_num_diff;
            ndropped_total += seq_num_diff - 1;
        }
        last_seq_num = seq_num;

        /* Determine if we go to next block */
        if ((seq_num>=nextblock_seq_num) || force_new_block) {

            /* Update drop stats */
            if (fblock->npacket)  
                drop_frac_avg = (1.0-drop_lpf)*drop_frac_avg 
                    + drop_lpf * 
                    (double)fblock->ndropped / 
                    (double)fblock->npacket;

            hashpipe_status_lock_safe(&st);
            hputi4(st.buf, "PKTIDX", fblock->packet_idx);                          
            hputr8(st.buf, "DROPAVG", drop_frac_avg);
            hputr8(st.buf, "DROPTOT", 
                    npacket_total ? 
                    (double)ndropped_total/(double)npacket_total 
                    : 0.0);
            hputr8(st.buf, "DROPBLK", 
                    fblock->npacket ? 
                    (double)fblock->ndropped/(double)fblock->npacket
                    : 0.0);
            hashpipe_status_unlock_safe(&st);

            /* Finalize first block, and push it off the list.
             * Then grab next available block.
             */
            if (fblock->block_idx>=0) finalize_block(fblock);
            block_stack_push(blocks, nblock);
            increment_block(lblock, seq_num);
            curdata = hpguppi_databuf_data(db, lblock->block_idx);
            curheader = hpguppi_databuf_header(db, lblock->block_idx);
            nextblock_seq_num = lblock->packet_idx 
                + packets_per_block - overlap_packets;

            /* If new obs started, reset total counters, get start
             * time.  Start time is rounded to nearest integer
             * second, with warning if we're off that by more
             * than 100ms.  Any current blocks on the stack
             * are also finalized/reset */
            if (force_new_block) {

                /* Reset stats */
                npacket_total=0;
                ndropped_total=0;
                nbogus_total=0;

                /* Get obs start time */
                get_current_mjd(&stt_imjd, &stt_smjd, &stt_offs);
                if (stt_offs>0.5) { stt_smjd+=1; stt_offs-=1.0; }
                if (fabs(stt_offs)>0.1) { 
                    char msg[256];
                    sprintf(msg, 
                            "Second fraction = %3.1f ms > +/-100 ms",
                            stt_offs*1e3);
                    hashpipe_warn("hpguppi_net_thread", msg);
                }
                stt_offs = 0.0;

                /* Warn if 1st packet number is not zero */
                if (seq_num!=0) {
                    char msg[256];
                    sprintf(msg, "First packet number is not 0 (seq_num=%lld)",
                            seq_num);
                    hashpipe_warn("hpguppi_net_thread", msg);
                }

                /* Flush any current buffers */
                for (i=0; i<nblock-1; i++) {
                    if (blocks[i].block_idx>=0) 
                        finalize_block(&blocks[i]);
                    reset_block(&blocks[i]);
                }

            }

            /* Read/update current status shared mem */
            hashpipe_status_lock_safe(&st);
            if (stt_imjd!=0) {
                hputi4(st.buf, "STT_IMJD", stt_imjd);
                hputi4(st.buf, "STT_SMJD", stt_smjd);
                hputr8(st.buf, "STT_OFFS", stt_offs);
                hputi4(st.buf, "STTVALID", 1);
            } else {
                // Put a non-accurate start time to avoid polyco 
                // errors.
                get_current_mjd(&stt_imjd, &stt_smjd, &stt_offs);
                hputi4(st.buf, "STT_IMJD", stt_imjd);
                hputi4(st.buf, "STT_SMJD", stt_smjd);
                hputi4(st.buf, "STTVALID", 0);
                // Reset to zero
                stt_imjd = 0;
                stt_smjd = 0;
            }
            memcpy(status_buf, st.buf, HASHPIPE_STATUS_TOTAL_SIZE);
            hashpipe_status_unlock_safe(&st);

            /* block size possibly changed on new obs */
            /* TODO: what about overlap...
             * Also, should this even be allowed ?
             */
            if (force_new_block) {
                if (hgeti4(status_buf, "BLOCSIZE", &block_size)==0) {
                        block_size = db->header.block_size;
                } else {
                    if (block_size > db->header.block_size) {
                        hashpipe_error("hpguppi_net_thread", 
                                "BLOCSIZE > databuf block_size");
                        block_size = db->header.block_size;
                    }
                }
                packets_per_block = block_size / packet_data_size;
            }
            hputi4(status_buf, "BLOCSIZE", block_size);

            /* Wait for new block to be free, then clear it
             * if necessary and fill its header with new values.
             */
            netbuf_full = hpguppi_input_databuf_total_status(db);
            sprintf(netbuf_status, "%d/%d", netbuf_full, db->header.n_block);
            hashpipe_status_lock_safe(&st);
            hputs(st.buf, status_key, "waitfree");
            hputs(st.buf, "NETBUFST", netbuf_status);
            hashpipe_status_unlock_safe(&st);
            while ((rv=hpguppi_input_databuf_wait_free(db, lblock->block_idx)) 
                    != HASHPIPE_OK) {
                if (rv==HASHPIPE_TIMEOUT) {
                    waiting=1;
                    netbuf_full = hpguppi_input_databuf_total_status(db);
                    sprintf(netbuf_status, "%d/%d", netbuf_full, db->header.n_block);
                    hashpipe_status_lock_safe(&st);
                    hputs(st.buf, status_key, "blocked");
                    hputs(st.buf, "NETBUFST", netbuf_status);
                    hashpipe_status_unlock_safe(&st);
                    continue;
                } else {
                    hashpipe_error("hpguppi_net_thread", 
                            "error waiting for free databuf");
                    pthread_exit(NULL);
                }
            }
            hashpipe_status_lock_safe(&st);
            hputs(st.buf, status_key, "receiving");
            hashpipe_status_unlock_safe(&st);

            memcpy(curheader, status_buf, HASHPIPE_STATUS_TOTAL_SIZE);
            //if (baseband_packets) { memset(curdata, 0, block_size); }
            if (1) { memset(curdata, 0, block_size); }

        }

        /* Copy packet into any blocks where it belongs.
         * The "write packets" functions also update drop stats 
         * for blocks, etc.
         */
        for (i=0; i<nblock; i++) {
            if ((blocks[i].block_idx>=0) 
                    && (block_packet_check(&blocks[i],seq_num)==0)) {
                if (baseband_packets) 
                    write_baseband_packet_to_block_from_pktsock_frame(
                        &blocks[i], p_frame, nchan);
                else
                    write_search_packet_to_block_from_pktsock_frame(
                        &blocks[i], p_frame);
            }
        }

        // Release frame back to ring buffer
        hashpipe_pktsock_release_frame(p_frame);

        /* Will exit if thread has been cancelled */
        pthread_testcancel();
    }

    pthread_exit(NULL);

    /* Have to close all push's */
    pthread_cleanup_pop(0); /* Closes hpguppi_free_psrfits */

    return NULL;
}

static hashpipe_thread_desc_t net_thread = {
    name: "hpguppi_net_thread",
    skey: "NETSTAT",
    init: init,
    run:  run,
    ibuf_desc: {NULL},
    obuf_desc: {hpguppi_input_databuf_create}
};

static __attribute__((constructor)) void ctor()
{
  register_hashpipe_thread(&net_thread);
}

// vi: set ts=8 sw=4 noet :
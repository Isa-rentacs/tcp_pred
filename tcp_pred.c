/*
 * Binary Increase Congestion control for TCP
 * Home page:
 *      http://netsrv.csc.ncsu.edu/twiki/bin/view/Main/BIC
 * This is from the implementation of BICTCP in
 * Lison-Xu, Kahaled Harfoush, and Injong Rhee.
 *  "Binary Increase Congestion Control for Fast, Long Distance
 *  Networks" in InfoComm 2004
 * Available from:
 *  http://netsrv.csc.ncsu.edu/export/bitcp.pdf
 *
 * Unless BIC is enabled and congestion window is large
 * this behaves the same as the original Reno.
 */

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/random.h>
#include <net/tcp.h>
#include "pow2.h"
#include "sigmoid.h"


#define BICTCP_BETA_SCALE    1024	/* Scale factor beta calculation
                                     * max_cwnd = snd_cwnd * beta
                                     */
#define BICTCP_B		4	 /*
                              * In binary search,
                              * go to point (max+min)/N
                              */

#define L 3
#define M 4
#define N 1
#define ETA 3
#define ALPHA 16
#define BETA 16
#define GAMMA 16
#define DELTA 16
#define LOOP_MAX 100
#define HIS_LEN 6 //number of teacher data

static int fast_convergence = 1;
static int max_increment = 16;
static int low_window = 14;
static int beta = 819;		/* = 819/1024 (BICTCP_BETA_SCALE) */
static int gamma = 1100;
static int initial_ssthresh;
static int smooth_part = 20;

module_param(fast_convergence, int, 0644);
MODULE_PARM_DESC(fast_convergence, "turn on/off fast convergence");
module_param(max_increment, int, 0644);
MODULE_PARM_DESC(max_increment, "Limit on increment allowed during binary search");
module_param(low_window, int, 0644);
MODULE_PARM_DESC(low_window, "lower bound on congestion window (for TCP friendliness)");
module_param(beta, int, 0644);
MODULE_PARM_DESC(beta, "beta for multiplicative increase");
module_param(gamma, int, 0644);
MODULE_PARM_DESC(beta, "gamma for multiplicative increase");
module_param(initial_ssthresh, int, 0644);
MODULE_PARM_DESC(initial_ssthresh, "initial value of slow start threshold");
module_param(smooth_part, int, 0644);
MODULE_PARM_DESC(smooth_part, "log(B/(B*Smin))/log(B/(B-1))+B, # of RTT from Wmax-B to Wmax");

/*parceptron parameters*/
struct perceptron_param{
    s64 wlm[L+1][M];
    s64 wmn[M+1][N];
    s64 dlm[L+1][M];
    s64 dmn[M+1][N];
    s64 Lout[L];
    s64 Min[M];
    s64 Mout[M];
    s64 Nin[N];
}p_param;

/* BIC TCP Parameters */
struct bictcp {
    u32	cnt;		/* increase cwnd by 1 after ACKs */
    u32 	last_max_cwnd;	/* last maximum snd_cwnd */
    u32	loss_cwnd;	/* congestion window at last loss */
    u32	last_cwnd;	/* the last snd_cwnd */
    u32	last_time;	/* time when updated last_cwnd */
    u32	epoch_start;	/* beginning of an epoch */
#define ACK_RATIO_SHIFT	4
    u32	delayed_ack;	/* estimate the ratio of Packets/ACKs << 4 */
#define NUMBER_OF_HISTORY 2 /* no meaning for default*/
    u16   elapsed[HIS_LEN];
    u16   rtt[HIS_LEN];
    u16   cwnd[HIS_LEN];
    u8    answer[HIS_LEN];
    u8    index;
    u8    his_num;
    u32   last_loss_time; /* time when previous packet loss */
};

static void initialize_perceptron(void){
    int i,j;
    for(i=0;i<L+1;i++){
        for(j=0;j<M;j++){
            p_param.wlm[i][j] = random32() % (pow2[DELTA+1]+1) - pow2[DELTA];
        }
    }
    for(i=0;i<M+1;i++){
        for(j=0;j<N;j++){
            p_param.wmn[i][j] = random32() % (pow2[DELTA+1]+1) - pow2[DELTA];
        }
    }
}

static void initialize_edge_delta(void){
    int i,j;
    for(i=0;i<L+1;i++){
        for(j=0;j<M;j++){
            p_param.dlm[i][j] = 0;
        }
    }
    for(i=0;i<M+1;i++){
        for(j=0;j<N;j++){
            p_param.dmn[i][j] = 0;
        }
    }
}

static s64 get_prediction(u16 elapsed, u16 srtt, u16 cwnd){
    s64 modin;
    int i,j;
    //L層の出力としてcaからデータを取る
    p_param.Lout[0] = elapsed;
    p_param.Lout[1] = srtt;
    p_param.Lout[2] = cwnd;

    //M層のi-thノードに対する入力値を計算する
    for(i=0;i<M;i++){
        p_param.Min[i] = 0;
        //Lout * weightの和を計算
        for(j=0;j<L;j++){
            p_param.Min[i] += p_param.wlm[j][i] * p_param.Lout[j];
        }
        //M層のi番目ノードの閾値分を入力から減算
        p_param.Min[i] += p_param.wlm[L][i] * -1;
    }

    //M層i-thノードのoutputを計算する    
    for(i=0;i<M;i++){
        modin = (p_param.Min[i] >> (1 + DELTA - ALPHA)) / BETA + pow2[ALPHA-1];
        if(0 <= modin && modin < (1 << ALPHA)){
            p_param.Mout[i] = sigmoid[modin];
        }else if(modin < 0){
            p_param.Mout[i] = 0;
        }else{
            p_param.Mout[i] = 1 << GAMMA;
        }
    }

    //N層i-thノードへの入力値を計算する
    for(i=0;i<N;i++){
        p_param.Nin[i] = 0;
        for(j=0;j<M;j++){
            //M層output * weightの和を計算
            p_param.Nin[i] += p_param.wmn[j][i] * p_param.Mout[j];
        }
        p_param.Nin[i] += p_param.wmn[M][i] * -1;
    }

    modin = (p_param.Nin[0] >> (1+GAMMA+DELTA-ALPHA)) / BETA + pow2[ALPHA-1];
    if(0 <= modin && modin < (1 << ALPHA)){
        return sigmoid[modin];
    }else if(modin < 0){
        return 0;
    }else{
        return 1 << GAMMA;
    }
}

static void train(struct bictcp *ca){
    s64 result, delta_k, delta_j;
    int x,i,j,k;
    int ans;

    initialize_perceptron();

    for(x=0;x<LOOP_MAX;x++){
        //差分変数の初期化
        initialize_edge_delta();

        //全ての教師データに対して
        for(i=0;i<ca->his_num;i++){
            //教師データを取得する必要がある
            ans = ca->answer[i];

            //予測を出す
            result = get_prediction(ca->elapsed[i],
                                    ca->rtt[i],
                                    ca->cwnd[i]);

            delta_k = (ans << GAMMA) - result;
            delta_k *= (1 << GAMMA) - result;
            delta_k >>= GAMMA;
            delta_k *= result;
            delta_k >>= GAMMA;

            //M->Nの偏微分値
            for(j=0;j<M+1;j++){
                for(k=0;k<N;k++){
                    if(j != M){
                        p_param.dmn[j][k] += (((delta_k * p_param.Mout[j]) >> GAMMA) << DELTA) >> GAMMA;
                    }else{
                        p_param.dmn[j][k] += ((delta_k * -1) << DELTA) >> GAMMA;
                    }
                }
            }

            //L->Mの偏微分値
            for(j=0;j<M;j++){
                delta_j = (delta_k * p_param.wmn[j][0]) >> DELTA;
                delta_j *= p_param.Mout[j];
                delta_j >>= GAMMA;
                delta_j *= (1<<GAMMA) - p_param.Mout[j];
                delta_j >>= GAMMA;
                for(k=0;k<L+1;k++){
                    if(k != L){
                        p_param.dlm[k][j] += (((delta_j * p_param.Lout[k]) >> GAMMA) << DELTA) >> GAMMA;
                    }else{
                        p_param.dlm[k][j] += ((delta_j * -1) << DELTA) >> GAMMA;
                    }
                }
            }
        }
        for(i=0;i<L+1;i++){
            for(j=0;j<M;j++){
                p_param.wlm[i][j] += p_param.dlm[i][j] >> ETA;
            }
        }
        for(i=0;i<M+1;i++){
            for(j=0;j<N;j++){
                p_param.wmn[i][j] += p_param.dmn[i][j] >> ETA;
            }
        }
    }
}

static inline void bictcp_reset(struct bictcp *ca)
{
    int i;
    ca->cnt = 0;
    ca->last_max_cwnd = 0;
    ca->loss_cwnd = 0;
    ca->last_cwnd = 0;
    ca->last_time = 0;
    ca->epoch_start = 0;
    ca->delayed_ack = 2 << ACK_RATIO_SHIFT;
    ca->last_loss_time = 0;
    ca->index = 0;
    ca->his_num = 0;
    for(i=0;i<HIS_LEN;i++){
        ca->elapsed[i] = 0;
        ca->rtt[i] = 0;
        ca->cwnd[i] = 0;
        ca->answer[i] = 0;
    }
}

static void bictcp_init(struct sock *sk)
{
    bictcp_reset(inet_csk_ca(sk));
    if (initial_ssthresh)
        tcp_sk(sk)->snd_ssthresh = initial_ssthresh;
}

/*
 * Compute congestion window to use.
 */
static inline void bictcp_update(struct bictcp *ca, u32 cwnd)
{
    if (ca->last_cwnd == cwnd &&
        (s32)(tcp_time_stamp - ca->last_time) <= HZ / 32)
        return;

    ca->last_cwnd = cwnd;
    ca->last_time = tcp_time_stamp;

    if (ca->epoch_start == 0) /* record the beginning of an epoch */
        ca->epoch_start = tcp_time_stamp;

    /* start off normal */
    if (cwnd <= low_window) {
        ca->cnt = cwnd;
        return;
    }

    /* binary increase */
    if (cwnd < ca->last_max_cwnd) {
        __u32 	dist = (ca->last_max_cwnd - cwnd)
            / BICTCP_B;

        if (dist > max_increment)
            /* linear increase */
            ca->cnt = cwnd / max_increment;
        else if (dist <= 1U){
            /* binary search increase */
            ca->cnt = (cwnd * smooth_part) / BICTCP_B;
        }
        else
            /* binary search increase */
            ca->cnt = cwnd / dist;
    } else {
        /* slow start AMD linear increase */
        if (cwnd < ca->last_max_cwnd + BICTCP_B)
            /* slow start */
            ca->cnt = (cwnd * smooth_part) / BICTCP_B;
        else if (cwnd < ca->last_max_cwnd + max_increment*(BICTCP_B-1))
            /* slow start */
            ca->cnt = (cwnd * (BICTCP_B-1))
                / (cwnd - ca->last_max_cwnd);
        else
            /* linear increase */
            ca->cnt = cwnd / max_increment;
    }

    /* if in slow start or link utilization is very low */
    if (ca->loss_cwnd == 0) {
        if (ca->cnt > 20) /* increase cwnd 5% per RTT */
            ca->cnt = 20;
    }

    ca->cnt = (ca->cnt << ACK_RATIO_SHIFT) / ca->delayed_ack;
    if (ca->cnt == 0)			/* cannot be zero */
        ca->cnt = 1;
}

static void bictcp_cong_avoid(struct sock *sk, u32 ack, u32 in_flight)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct bictcp *ca = inet_csk_ca(sk);

    if (!tcp_is_cwnd_limited(sk, in_flight))
        return;

    if (tp->snd_cwnd <= tp->snd_ssthresh)
        tcp_slow_start(tp);
    else {
        bictcp_update(ca, tp->snd_cwnd);
        tcp_cong_avoid_ai(tp, ca->cnt);
    }

}

/*
 *	behave like Reno until low_window is reached,
 *	then increase congestion window slowly
 */
/*
 *      NOTE:this function is called when a packet was dropped.
 *      the reason is this code "ca->loss_cwnd = tp->snd_cwnd;"
 */
static u32 bictcp_recalc_ssthresh(struct sock *sk)
{
    const struct tcp_sock *tp = tcp_sk(sk);
    struct bictcp *ca = inet_csk_ca(sk);
    u16 port=0;
    u32 buf_last_max_cwnd, prediction;
    ca->epoch_start = 0;	/* end of epoch */


    //store last_max_cwnd
    buf_last_max_cwnd = ca->last_max_cwnd;

    port = (u16)tp->inet_conn.icsk_inet.inet_sport >> 8;
    port += (u16)tp->inet_conn.icsk_inet.inet_sport << 8;
    if(tp->snd_cwnd < ca->last_max_cwnd){
        printk("[L%d]%d %d %d %d %d 0\n", port, tcp_time_stamp - ca->last_loss_time, tp->srtt, ca->last_max_cwnd, tp->snd_ssthresh, ca->loss_cwnd);
    }else{
        printk("[L%d]%d %d %d %d %d 1\n", port, tcp_time_stamp - ca->last_loss_time, tp->srtt, ca->last_max_cwnd, tp->snd_ssthresh, ca->loss_cwnd);
    }

    /* Wmax and fast convergence */
    if(ca->his_num == 0){ 
        /*
         * if there are no histories of packet loss,
         * act as default BIC.
         */
        if (tp->snd_cwnd < ca->last_max_cwnd && fast_convergence)
            ca->last_max_cwnd = (tp->snd_cwnd * (BICTCP_BETA_SCALE + beta))
                / (2 * BICTCP_BETA_SCALE);
        else
            ca->last_max_cwnd = tp->snd_cwnd;
    }else{
        /*
         * in case there is at least one history
         *
         */
        train(ca);
        prediction = get_prediction(tcp_time_stamp - ca->last_loss_time,
                                           tp->srtt,
                                           tp->snd_cwnd);
        printk("[tcp_pred] packet lossed predction = %d, his_num = %d\n", prediction, ca->his_num);
        if(prediction < (1 << (GAMMA - 1))){
            ca->last_max_cwnd = (tp->snd_cwnd * (BICTCP_BETA_SCALE + beta))
                / (2 * BICTCP_BETA_SCALE);
        }else{
            ca->last_max_cwnd = tp->snd_cwnd;
        }
    }
    //default action
    ca->loss_cwnd = tp->snd_cwnd;
    //index番目にloss状況を記録
    ca->elapsed[ca->index] = tcp_time_stamp - ca->last_loss_time;
    ca->rtt[ca->index] = tp->srtt;
    ca->cwnd[ca->index] = tp->snd_cwnd;
    if(tp->snd_cwnd < buf_last_max_cwnd){
        ca->answer[ca->index] = 0;
    }else{
        ca->answer[ca->index] = 1;
    }

    //indexを1つ進める
    ca->index++;
    if(ca->index == HIS_LEN){
        if(ca->ready == 0){
            ca->ready = 1;
        }
        ca->index = 0;
    }

    //his_num++
    if(ca->his_num < HIS_LEN){
        ca->his_num++;
    }

    //update timestamp at a packet loss
    ca->last_loss_time = tcp_time_stamp;

    if (tp->snd_cwnd <= low_window)
        return max(tp->snd_cwnd >> 1U, 2U);
    else
        return max((tp->snd_cwnd * beta) / BICTCP_BETA_SCALE, 2U);
}

static u32 bictcp_undo_cwnd(struct sock *sk)
{
    const struct tcp_sock *tp = tcp_sk(sk);
    const struct bictcp *ca = inet_csk_ca(sk);
    return max(tp->snd_cwnd, ca->last_max_cwnd);
}

static void bictcp_state(struct sock *sk, u8 new_state)
{
    if (new_state == TCP_CA_Loss)
        bictcp_reset(inet_csk_ca(sk));
}

/* Track delayed acknowledgment ratio using sliding window
 * ratio = (15*ratio + sample) / 16
 */
static void bictcp_acked(struct sock *sk, u32 cnt, s32 rtt)
{
    const struct inet_connection_sock *icsk = inet_csk(sk);

    if (icsk->icsk_ca_state == TCP_CA_Open) {
        struct bictcp *ca = inet_csk_ca(sk);
        cnt -= ca->delayed_ack >> ACK_RATIO_SHIFT;
        ca->delayed_ack += cnt;
    }
}


static struct tcp_congestion_ops bictcp = {
    .init		= bictcp_init,
    .ssthresh	= bictcp_recalc_ssthresh,
    .cong_avoid	= bictcp_cong_avoid,
    .set_state	= bictcp_state,
    .undo_cwnd	= bictcp_undo_cwnd,
    .pkts_acked = bictcp_acked,
    .owner		= THIS_MODULE,
    .name		= "tcp_pred",
};

static int __init bictcp_register(void)
{
    BUILD_BUG_ON(sizeof(struct bictcp) > ICSK_CA_PRIV_SIZE);
    return tcp_register_congestion_control(&bictcp);
}

static void __exit bictcp_unregister(void)
{
    tcp_unregister_congestion_control(&bictcp);
}

module_init(bictcp_register);
module_exit(bictcp_unregister);

MODULE_AUTHOR("Stephen Hemminger");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("BIC TCP");

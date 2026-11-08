# summarize.awk - median/min/max over repeated SC-LDPC benchmark runs.
#
# Input: runs CSV with header
#   batch,round,variant,threads,block,L,rate,p,seed,load1,trials,
#   recovery,ci99,iter,enc,dec,latency_ms,mem_bytes,order
#
# Output (stdout): three CSV sections, separated by a blank line:
#   1. long metrics: batch,variant,p,rate,block,L,threads,n,metric,median,min,max
#   2. wide summary: batch,variant,p,rate,block,L,threads,n,
#                    recovery,iter,enc,dec,
#                    recovery_min,recovery_max,iter_min,iter_max,
#                    enc_min,enc_max,dec_min,dec_max
#   3. paired A/B:   batch,cand,p,rate,block,L,threads,n,
#                    base_enc,cand_enc,enc_ratio,enc_ratio_min,enc_ratio_max,
#                    base_dec,cand_dec,dec_ratio,dec_ratio_min,dec_ratio_max,
#                    base_rec,cand_rec,rec_max_abs_delta,
#                    base_iter,cand_iter,iter_max_abs_delta
#
# Use -v cand=<variant> to name the candidate.  base is always "base".
# Ratios are per-round paired cand/base; median over rounds.

BEGIN {
    FS = ","
    OFS = ","
    nk = 0
    npt = 0
}

NR == 1 {
    for (i = 1; i <= NF; i++)
        h[$i] = i
    next
}

{
    b = $h["batch"]
    v = $h["variant"]
    th = $h["threads"]
    blk = $h["block"]
    L = $h["L"]
    rate = $h["rate"]
    p = $h["p"]
    rnd = $h["round"]

    if (b0 == "")
        b0 = b

    key = v SUBSEP p SUBSEP rate SUBSEP blk SUBSEP L SUBSEP th
    if (!(key in seen)) {
        seen[key] = 1
        order[++nk] = key
    }
    n[key]++

    i = n[key]
    m[key, i, "recovery"] = $h["recovery"] + 0
    m[key, i, "iter"]     = $h["iter"] + 0
    m[key, i, "enc"]      = $h["enc"] + 0
    m[key, i, "dec"]      = $h["dec"] + 0

    pt = p SUBSEP rate SUBSEP blk SUBSEP L SUBSEP th
    if (!(pt in ptseen)) {
        ptseen[pt] = 1
        ptorder[++npt] = pt
    }
    pv[pt, rnd, v, "enc"]      = $h["enc"] + 0
    pv[pt, rnd, v, "dec"]      = $h["dec"] + 0
    pv[pt, rnd, v, "recovery"] = $h["recovery"] + 0
    pv[pt, rnd, v, "iter"]     = $h["iter"] + 0
    if (!((pt, rnd) in rseen)) {
        rseen[pt, rnd] = 1
    }
}

function median(key, metric, n,   a, i, j, t) {
    for (i = 1; i <= n; i++)
        a[i] = m[key, i, metric]
    for (i = 2; i <= n; i++) {
        t = a[i]
        for (j = i - 1; j >= 1 && a[j] > t; j--)
            a[j + 1] = a[j]
        a[j + 1] = t
    }
    if (n % 2)
        return a[(n + 1) / 2]
    return (a[n / 2] + a[n / 2 + 1]) / 2
}

function vmin(key, metric, n,   i, x) {
    x = m[key, 1, metric]
    for (i = 2; i <= n; i++)
        if (m[key, i, metric] < x)
            x = m[key, i, metric]
    return x
}

function vmax(key, metric, n,   i, x) {
    x = m[key, 1, metric]
    for (i = 2; i <= n; i++)
        if (m[key, i, metric] > x)
            x = m[key, i, metric]
    return x
}

function median_arr(a, n,   b, i, j, t) {
    for (i = 1; i <= n; i++)
        b[i] = a[i]
    for (i = 2; i <= n; i++) {
        t = b[i]
        for (j = i - 1; j >= 1 && b[j] > t; j--)
            b[j + 1] = b[j]
        b[j + 1] = t
    }
    if (n % 2)
        return b[(n + 1) / 2]
    return (b[n / 2] + b[n / 2 + 1]) / 2
}

END {
    print "batch,variant,p,rate,block,L,threads,n,metric,median,min,max"
    for (k = 1; k <= nk; k++) {
        key = order[k]
        split(key, kp, SUBSEP)
        nrun = n[key]
        for (mi = 1; mi <= 4; mi++) {
            metric = (mi == 1 ? "recovery" : mi == 2 ? "iter" : mi == 3 ? "enc" : "dec")
            printf "%s,%s,%s,%s,%s,%s,%s,%d,%s,%.6f,%.6f,%.6f\n", \
                b0, kp[1], kp[2], kp[3], kp[4], kp[5], kp[6], nrun, metric, \
                median(key, metric, nrun), vmin(key, metric, nrun), vmax(key, metric, nrun)
        }
    }

    print ""
    print "batch,variant,p,rate,block,L,threads,n,recovery,iter,enc,dec,recovery_min,recovery_max,iter_min,iter_max,enc_min,enc_max,dec_min,dec_max"
    for (k = 1; k <= nk; k++) {
        key = order[k]
        split(key, kp, SUBSEP)
        nrun = n[key]
        printf "%s,%s,%s,%s,%s,%s,%s,%d,%.6f,%.2f,%.3f,%.3f,%.6f,%.6f,%.2f,%.2f,%.3f,%.3f,%.3f,%.3f\n", \
            b0, kp[1], kp[2], kp[3], kp[4], kp[5], kp[6], nrun, \
            median(key, "recovery", nrun), median(key, "iter", nrun), \
            median(key, "enc", nrun), median(key, "dec", nrun), \
            vmin(key, "recovery", nrun), vmax(key, "recovery", nrun), \
            vmin(key, "iter", nrun), vmax(key, "iter", nrun), \
            vmin(key, "enc", nrun), vmax(key, "enc", nrun), \
            vmin(key, "dec", nrun), vmax(key, "dec", nrun)
    }

    print ""
    print "batch,cand,p,rate,block,L,threads,n,base_enc,cand_enc,enc_ratio,enc_ratio_min,enc_ratio_max,base_dec,cand_dec,dec_ratio,dec_ratio_min,dec_ratio_max,base_rec,cand_rec,rec_max_abs_delta,base_iter,cand_iter,iter_max_abs_delta"
    for (pi = 1; pi <= npt; pi++) {
        pt = ptorder[pi]
        split(pt, pp, SUBSEP)
        # paired rounds: both variants present
        nr = 0
        delete er; delete dr
        recmax = 0
        itermax = 0
        havebase = 0
        havecand = 0
        for (r = 1; r <= 1000; r++) {
            if (!((pt, r) in rseen))
                continue
            bk = pt SUBSEP r SUBSEP "base" SUBSEP "enc"
            ck = pt SUBSEP r SUBSEP cand SUBSEP "enc"
            if (!(bk in pv) || !(ck in pv))
                continue
            nr++
            be = pv[pt, r, "base", "enc"]
            ce = pv[pt, r, cand, "enc"]
            bd = pv[pt, r, "base", "dec"]
            cd = pv[pt, r, cand, "dec"]
            br = pv[pt, r, "base", "recovery"]
            cr = pv[pt, r, cand, "recovery"]
            bi = pv[pt, r, "base", "iter"]
            ci = pv[pt, r, cand, "iter"]
            er[nr] = ce / be
            dr[nr] = cd / bd
            d = cr - br
            if (d < 0)
                d = -d
            if (d > recmax)
                recmax = d
            d = ci - bi
            if (d < 0)
                d = -d
            if (d > itermax)
                itermax = d
            lastbe = be; lastce = ce
            lastbd = bd; lastcd = cd
            lastbr = br; lastcr = cr
            lastbi = bi; lastci = ci
            havebase = 1
            havecand = 1
        }
        if (!havebase || !havecand || nr == 0)
            continue
        bkey = "base" SUBSEP pp[1] SUBSEP pp[2] SUBSEP pp[3] SUBSEP pp[4] SUBSEP pp[5]
        ckey = cand SUBSEP pp[1] SUBSEP pp[2] SUBSEP pp[3] SUBSEP pp[4] SUBSEP pp[5]
        printf "%s,%s,%s,%s,%s,%s,%s,%d,%.3f,%.3f,%.4f,%.4f,%.4f,%.3f,%.3f,%.4f,%.4f,%.4f,%.6f,%.6f,%.6f,%.2f,%.2f,%.2f\n", \
            b0, cand, pp[1], pp[2], pp[3], pp[4], pp[5], nr, \
            median(bkey, "enc", n[bkey]), median(ckey, "enc", n[ckey]), \
            median_arr(er, nr), vmin_arr(er, nr), vmax_arr(er, nr), \
            median(bkey, "dec", n[bkey]), median(ckey, "dec", n[ckey]), \
            median_arr(dr, nr), vmin_arr(dr, nr), vmax_arr(dr, nr), \
            median(bkey, "recovery", n[bkey]), median(ckey, "recovery", n[ckey]), \
            recmax, \
            median(bkey, "iter", n[bkey]), median(ckey, "iter", n[ckey]), itermax
    }
}

function vmin_arr(a, n,   i, x) {
    x = a[1]
    for (i = 2; i <= n; i++)
        if (a[i] < x)
            x = a[i]
    return x
}

function vmax_arr(a, n,   i, x) {
    x = a[1]
    for (i = 2; i <= n; i++)
        if (a[i] > x)
            x = a[i]
    return x
}

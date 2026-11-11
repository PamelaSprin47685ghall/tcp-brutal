# 上行 8×32 GF(16) SC-LDPC：构造边界与解析恢复界

状态：大规模扫描停止；scan/wake/ge/carrier 模式保留不扩展，默认目标改为构造枚举+解析式+1000 超块抽查。
## 1. 构造
真实构造器（block=128,L=16,m=32-n,rate_num=8n,BITPLANE64）枚举 n=1..31：可构造 {1,2,4..18,21..25,28,30}，失败 {3,19,20,26,27,29,31}。
约束：① codes=16n、n_data=1024，各批 8192 B 精确填满（n=3 单批 48 lanes、8192%24≠0；n=7/11/15/23 多批可拼满，源码“48 lanes 拒绝”注释过严）；
② codes*n_par≤m*16384，n>16 时 n_par 按 64 取整，19/20/26/27/29/31 向上取整超预算；③ groups*(n+m)=256，groups≤32。
wire_of_cfg 的纯档位检查：rate_ppm∈[250000,700000]、rate_ppm*shares/1e6==n、rate_index_for_shape 命中 6 档；纯格式检查：上行 shares==32/block==128/L==16/groups>1；
编解码器可建 24 个 n 与宽度 16/64/128，wire 只放行 6 档。
## 2. 解析界
码字 N=n_data+n_par，n_par 个校验；缺 k_d 数据份、k_p 奇偶份擦除 u=k_d*n_data/n+k_p*n_par/m 个符号；MDS 份阈值 m_eff=floor(n_par*n/n_data)。
秩判定：满秩概率 ∏_{i<u}(1-16^{i-r})，亏损概率 ≈16^{-(r-u+1)}；k*=n_par*n/n_data，k<k* 时指数=(k*-k)*n_data/n+1，k>k* 欠定，k*∈Z 时 k=k* 以 1/16 失败；阶梯上 Δ_rank=m-⌊k*⌋+[k*∈Z]=1。
peeling：可解 ⇔ 擦除子图无 stopping set；停滞核≤max_core=256 且核矩阵满秩才由消元补救；整个擦除进核的上界 k_core=floor(256*32/N)，Δ_core=m-k_core。
随机 GF(16) 界只是乐观下界；真实 Δ 由 SC-IRA 停止集决定：低码率远高于 k_core，高码率贴近 k_core。
| pct | n | m | m_eff | Δmds | Δrank | k_core | Δcore |
|---|---|---|---|---|---|---|---|
| 25 | 8 | 24 | 24 | 0 | 1 | 2 | 22 |
| 30 | 9 | 23 | 22 | 1 | 1 | 2 | 21 |
| 40 | 12 | 20 | 19 | 1 | 1 | 3 | 17 |
| 50 | 16 | 16 | 16 | 0 | 1 | 4 | 12 |
| 60 | 18 | 14 | 13 | 1 | 1 | 4 | 10 |
| 70 | 22 | 10 | 9 | 1 | 1 | 5 | 5 |
抽查：n=8 p=0.5 组失败 2.1%/超块 16%；n=22 p=0.1 组失败 1.4%/超块 10.4%；核上限保持 256，70% 档这 1.4% 的恢复缺口由连续控制器的在线 Δ 校正吸收（不抬核上限）。
## 3. 组合
独立：P_super=1-(1-P_g)^8≈8P_g；完全相关：P_super=P_g；任意相关：P_g≤P_super≤min(1,8P_g)。
死 carrier 的每组件数超几何（负相关），P_super 略低于独立值；共同突发正相关，趋向 P_g；控制器 p 只统计存活 carrier 的 data 份丢失，相关失效时低估，须用 P_super。
## 4. 连续控制
粒度 1/32=3.125pp > 2pp 滞回：一次换档边界移动 3.125pp，控制器无法把边界放在两档之间，只能取低档，损失≤3.125pp。
30/60/70 档 wire 的 rate_ppm（0.300/0.593/0.696）≠实际份率 n/32（0.281/0.563/0.688），1-rate_ppm-0.2 再引入最多 3pp 偏差；20% 固定余量应改为按档阈值减 2pp 滞回。
8 组各用不同 n：wire 只带一个 n/m，映射与估计器按均匀份数设计；P_super≈ΣP_g 由最差组主导，平均码率不改善可靠性；单流无益，需改 wire。
换组宽：w=64/128 粒度 1.56/0.78pp；N 由码率决定，w 只改份数方差、codes=16n、块大小与 CPU，超块仍 256 份，一阶 P_super 与 w 无关；w=64 可选，w=32 为当前折中。
## 5. 复现
make host-calib-sclc-up && build/bin/calib_sclc_up construct && build/bin/calib_sclc_up analyze
build/bin/calib_sclc_up spot --level 70 --trials 1000 [--core 512]；源码 tests/bench/calib_sclc_up.c，数据 tests/calib/sclc-up.csv，分支 w-calib-up。

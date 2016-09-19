/*
 * Copyright 2011, Ben Langmead <langmea@cs.jhu.edu>
 *
 * This file is part of Bowtie 2.
 *
 * Bowtie 2 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Bowtie 2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Bowtie 2.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string>
#include <sys/time.h>
#include "sam.h"
#include "filebuf.h"

using namespace std;

/**
 * Print a reference name in a way that doesn't violate SAM's character
 * constraints. \*|[!-()+-<>-~][!-~]* (i.e. [33, 63], [65, 126])
 */
void SamConfig::printRefName(
	BTString& o,
	const std::string& name) const
{
	size_t namelen = name.length();
	for(size_t i = 0; i < namelen; i++) {
		if(isspace(name[i])) {
			return;
		}
		o.append(name[i]);
	}
}

/**
 * Print a reference name given a reference index.
 */
void SamConfig::printRefNameFromIndex(BTString& o, size_t i) const {
	printRefName(o, refnames_[i]);
}

/**
 * Print SAM header to given output buffer.
 */
void SamConfig::printHeader(
	BTString& o,
	const string& rgid,
	const string& rgs,
	bool printHd,
	bool printSq,
	bool printPg) const
{
	if(printHd) printHdLine(o, "1.0");
	if(printSq) printSqLines(o);
	if(!rgid.empty()) {
		o.append("@RG");
		o.append(rgid.c_str());
		o.append(rgs.c_str());
		o.append('\n');
	}
	if(printPg) printPgLine(o);
}

/**
 * Print the @HD header line to the given string.
 */
void SamConfig::printHdLine(BTString& o, const char *samver) const {
	o.append("@HD\tVN:");
	o.append(samver);
	o.append("\tSO:unsorted\n");
}

/**
 * Print the @SQ header lines to the given string.
 */
void SamConfig::printSqLines(BTString& o) const {
	char buf[1024];
	for(size_t i = 0; i < refnames_.size(); i++) {
		o.append("@SQ\tSN:");
		printRefName(o, refnames_[i]);
		o.append("\tLN:");
		itoa10<size_t>(reflens_[i], buf);
		o.append(buf);
		o.append('\n');
	}
}

/**
 * Print the @PG header line to the given string.
 */
void SamConfig::printPgLine(BTString& o) const {
	o.append("@PG\tID:");
	o.append(pg_id_.c_str());
	o.append("\tPN:");
	o.append(pg_pn_.c_str());
	o.append("\tVN:");
	o.append(pg_vn_.c_str());
	o.append("\tCL:\"");
	o.append(pg_cl_.c_str());
	o.append('"');
	o.append('\n');
}

#define WRITE_SEP() { \
	if(!first) o.append('\t'); \
	first = false; \
}

/**
 * Print the optional flags to the given string.
 */
void SamConfig::printAlignedOptFlags(
	BTString& o,               // output buffer
	bool first,                // first opt flag printed is first overall?
	const Read& rd,            // the read
	const Read* rdo,           // the opposite read
	AlnRes& res,               // individual alignment result
	StackedAln& staln,         // stacked alignment buffer
	const AlnFlags& flags,     // alignment flags
	const AlnSetSumm& summ,    // summary of alignments for this read
	const SeedAlSumm& ssm,     // seed alignment summary
	const PerReadMetrics& prm, // per-read metrics
	const Scoring& sc,         // scoring scheme
	const char *mapqInp)       // inputs to MAPQ calculation
	const
{
	char buf[1024];
	assert(summ.best(rd.mate < 2).valid());
	if(print_as_) {
		// AS:i: Alignment score generated by aligner
		itoa10<TAlScore>(res.score().score(), buf);
		WRITE_SEP();
		o.append("AS:i:");
		o.append(buf);
	}
	if(print_xs_) {
		// XS:i: Suboptimal alignment score
		AlnScore sco;
		if(flags.alignedConcordant()) {
			sco = summ.bestUnchosen(rd.mate < 2);
		} else {
			sco = summ.secbest(rd.mate < 2);
		}
		if(sco.valid()) {
			itoa10<TAlScore>(sco.score(), buf);
			WRITE_SEP();
			o.append("XS:i:");
			o.append(buf);
		}
	}
	if(print_xn_) {
		// XN:i: Number of ambiguous bases in the referenece
		itoa10<size_t>(res.refNs(), buf);
		WRITE_SEP();
		o.append("XN:i:");
		o.append(buf);
	}
	if(print_x0_) {
		// X0:i: Number of best hits
	}
	if(print_x1_) {
		// X1:i: Number of sub-optimal best hits
	}
	size_t num_mm = 0;
	size_t num_go = 0;
	size_t num_gx = 0;
	for(size_t i = 0; i < res.ned().size(); i++) {
		if(res.ned()[i].isMismatch()) {
			num_mm++;
		} else if(res.ned()[i].isReadGap()) {
			num_go++;
			num_gx++;
			while(i < res.ned().size()-1 &&
				  res.ned()[i+1].pos == res.ned()[i].pos &&
				  res.ned()[i+1].isReadGap())
			{
				i++;
				num_gx++;
			}
		} else if(res.ned()[i].isRefGap()) {
			num_go++;
			num_gx++;
			while(i < res.ned().size()-1 &&
				  res.ned()[i+1].pos == res.ned()[i].pos+1 &&
				  res.ned()[i+1].isRefGap())
			{
				i++;
				num_gx++;
			}
		}
	}
	if(print_xm_) {
		// XM:i: Number of mismatches in the alignment
		itoa10<size_t>(num_mm, buf);
		WRITE_SEP();
		o.append("XM:i:");
		o.append(buf);
	}
	if(print_xo_) {
		// XO:i: Number of gap opens
		itoa10<size_t>(num_go, buf);
		WRITE_SEP();
		o.append("XO:i:");
		o.append(buf);
	}
	if(print_xg_) {
		// XG:i: Number of gap extensions (incl. opens)
		itoa10<size_t>(num_gx, buf);
		WRITE_SEP();
		o.append("XG:i:");
		o.append(buf);
	}
	if(print_nm_) {
		// NM:i: Edit dist. to the ref, Ns count, clipping doesn't
		itoa10<size_t>(res.ned().size(), buf);
		WRITE_SEP();
		o.append("NM:i:");
		o.append(buf);
	}
	if(print_md_) {
		// MD:Z: String for mms. [0-9]+(([A-Z]|\^[A-Z]+)[0-9]+)*2
		WRITE_SEP();
		o.append("MD:Z:");
		staln.buildMdz();
		staln.writeMdz(
			&o,        // output buffer
			NULL);     // no char buffer
	}
	if(print_ys_ && summ.paired()) {
		// YS:i: Alignment score of opposite mate
		assert(res.oscore().valid());
		itoa10<TAlScore>(res.oscore().score(), buf);
		WRITE_SEP();
		o.append("YS:i:");
		o.append(buf);
	}
	if(print_yn_) {
		// YN:i: Minimum valid score for this mate
		TAlScore mn = sc.scoreMin.f<TAlScore>(rd.length());
		itoa10<TAlScore>(mn, buf);
		WRITE_SEP();
		o.append("YN:i:");
		o.append(buf);
		// Yn:i: Perfect score for this mate
		TAlScore pe = sc.perfectScore(rd.length());
		itoa10<TAlScore>(pe, buf);
		WRITE_SEP();
		o.append("Yn:i:");
		o.append(buf);
		if(summ.paired()) {
			assert(rdo != NULL);
			// ZN:i: Minimum valid score for opposite mate
			TAlScore mn = sc.scoreMin.f<TAlScore>(rdo->length());
			itoa10<TAlScore>(mn, buf);
			WRITE_SEP();
			o.append("ZN:i:");
			o.append(buf);
			// Zn:i: Perfect score for opposite mate
			TAlScore pe = sc.perfectScore(rdo->length());
			itoa10<TAlScore>(pe, buf);
			WRITE_SEP();
			o.append("Zn:i:");
			o.append(buf);
		}
	}
	if(print_xss_) {
		// Xs:i: Best invalid alignment score of this mate
		bool one = true;
		if(flags.partOfPair() && !flags.readMate1()) {
			one = false;
		}
		TAlScore bst = one ? prm.bestLtMinscMate1 : prm.bestLtMinscMate2;
		if(bst > std::numeric_limits<TAlScore>::min()) {
			itoa10<TAlScore>(bst, buf);
			WRITE_SEP();
			o.append("Xs:i:");
			o.append(buf);
		}
		if(flags.partOfPair()) {
			// Ys:i: Best invalid alignment score of opposite mate
			bst = one ? prm.bestLtMinscMate2 : prm.bestLtMinscMate1;
			if(bst > std::numeric_limits<TAlScore>::min()) {
				itoa10<TAlScore>(bst, buf);
				WRITE_SEP();
				o.append("Ys:i:");
				o.append(buf);
			}
		}
	}
	if(print_zs_) {
		// ZS:i: Pseudo-random seed for read
		itoa10<uint32_t>(rd.seed, buf);
		WRITE_SEP();
		o.append("ZS:i:");
		o.append(buf);
	}
	if(print_yt_) {
		// YT:Z: String representing alignment type
		WRITE_SEP();
		flags.printYT(o);
	}
	if(print_yp_ && flags.partOfPair() && flags.canMax()) {
		// YP:i: Read was repetitive when aligned paired?
		WRITE_SEP();
		flags.printYP(o);
	}
	if(print_ym_ && flags.canMax() && (flags.isMixedMode() || !flags.partOfPair())) {
		// YM:i: Read was repetitive when aligned unpaired?
		WRITE_SEP();
		flags.printYM(o);
	}
	if(print_yf_ && flags.filtered()) {
		// YF:i: Read was filtered?
		first = flags.printYF(o, first) && first;
	}
	if(print_yi_) {
		// Print MAPQ calibration info
		if(mapqInp[0] != '\0') {
			// YI:i: Suboptimal alignment score
			WRITE_SEP();
			o.append("YI:Z:");
			o.append(mapqInp);
		}
	}
	if(flags.partOfPair() && print_zp_) {
		// ZP:i: Score of best concordant paired-end alignment
		if(summ.bestPaired().valid()) {
			WRITE_SEP();
			o.append("ZP:i:");
			itoa10<TAlScore>(summ.bestPaired().score(), buf);
			o.append(buf);
		}
		// Zp:i: Score of second-best concordant paired-end alignment
		if(summ.secbestPaired().valid()) {
			WRITE_SEP();
			o.append("Zp:i:");
			itoa10<TAlScore>(summ.secbestPaired().score(), buf);
			o.append(buf);
		}
	}
	if(print_zu_) {
		// ZU:i: Score of best unpaired alignment
		AlnScore best    = (rd.mate <= 1 ? summ.best1()    : summ.best2());
		AlnScore secbest = (rd.mate <= 1 ? summ.secbest1() : summ.secbest2());
		WRITE_SEP();
		o.append("ZU:i:");
		if(best.valid()) {
			itoa10<TAlScore>(best.score(), buf);
			o.append(buf);
		} else {
			o.append("NA");
		}
		// Zu:i: Score of second-best unpaired alignment
		WRITE_SEP();
		o.append("Zu:i:");
		if(secbest.valid()) {
			itoa10<TAlScore>(secbest.score(), buf);
			o.append(buf);
		} else {
			o.append("NA");
		}
	}
	if(!rgs_.empty()) {
		WRITE_SEP();
		o.append(rgs_.c_str());
	}
	if(print_xt_) {
		// XT:i: Timing
		WRITE_SEP();
		struct timeval  tv_end;
		struct timezone tz_end;
		gettimeofday(&tv_end, &tz_end);
		size_t total_usecs =
			(tv_end.tv_sec  - prm.tv_beg.tv_sec) * 1000000 +
			(tv_end.tv_usec - prm.tv_beg.tv_usec);
		itoa10<size_t>(total_usecs, buf);
		o.append("XT:i:");
		o.append(buf);
	}
	if(print_xd_) {
		// XD:i: Extend DPs
		WRITE_SEP();
		itoa10<uint64_t>(prm.nExDps, buf);
		o.append("XD:i:");
		o.append(buf);
		// Xd:i: Mate DPs
		WRITE_SEP();
		itoa10<uint64_t>(prm.nMateDps, buf);
		o.append("Xd:i:");
		o.append(buf);
	}
	if(print_xu_) {
		// XU:i: Extend ungapped tries
		WRITE_SEP();
		itoa10<uint64_t>(prm.nExUgs, buf);
		o.append("XU:i:");
		o.append(buf);
		// Xu:i: Mate ungapped tries
		WRITE_SEP();
		itoa10<uint64_t>(prm.nMateUgs, buf);
		o.append("Xu:i:");
		o.append(buf);
	}
	if(print_ye_) {
		// YE:i: Streak of failed DPs at end
		WRITE_SEP();
		itoa10<uint64_t>(prm.nDpFail, buf);
		o.append("YE:i:");
		o.append(buf);
		// Ye:i: Streak of failed ungaps at end
		WRITE_SEP();
		itoa10<uint64_t>(prm.nUgFail, buf);
		o.append("Ye:i:");
		o.append(buf);
	}
	if(print_yl_) {
		// YL:i: Longest streak of failed DPs
		WRITE_SEP();
		itoa10<uint64_t>(prm.nDpFailStreak, buf);
		o.append("YL:i:");
		o.append(buf);
		// Yl:i: Longest streak of failed ungaps
		WRITE_SEP();
		itoa10<uint64_t>(prm.nUgFailStreak, buf);
		o.append("Yl:i:");
		o.append(buf);
	}
	if(print_yu_) {
		// YU:i: Index of last succesful DP
		WRITE_SEP();
		itoa10<uint64_t>(prm.nDpLastSucc, buf);
		o.append("YU:i:");
		o.append(buf);
		// Yu:i: Index of last succesful DP
		WRITE_SEP();
		itoa10<uint64_t>(prm.nUgLastSucc, buf);
		o.append("Yu:i:");
		o.append(buf);
	}
	if(print_xp_) {
		// XP:Z: String describing seed hits
		WRITE_SEP();
		o.append("XP:B:I,");
		itoa10<uint64_t>(prm.nSeedElts, buf);
		o.append(buf);
		o.append(',');
		itoa10<uint64_t>(prm.nSeedEltsFw, buf);
		o.append(buf);
		o.append(',');
		itoa10<uint64_t>(prm.nSeedEltsRc, buf);
		o.append(buf);
		o.append(',');
		itoa10<uint64_t>(prm.seedMean, buf);
		o.append(buf);
		o.append(',');
		itoa10<uint64_t>(prm.seedMedian, buf);
		o.append(buf);
	}
	if(print_yr_) {
		// YR:i: Redundant seed hits
		WRITE_SEP();
		itoa10<uint64_t>(prm.nRedundants, buf);
		o.append("YR:i:");
		o.append(buf);
	}
	if(print_zb_) {
		// ZB:i: Ftab ops for seed alignment
		WRITE_SEP();
		itoa10<uint64_t>(prm.nFtabs, buf);
		o.append("ZB:i:");
		o.append(buf);
	}
	if(print_zr_) {
		// ZR:Z: Redundant path skips in seed alignment
		WRITE_SEP();
		o.append("ZR:Z:");
		itoa10<uint64_t>(prm.nRedSkip, buf); o.append(buf);
		o.append(',');
		itoa10<uint64_t>(prm.nRedFail, buf); o.append(buf);
		o.append(',');
		itoa10<uint64_t>(prm.nRedIns, buf); o.append(buf);
	}
	if(print_zf_) {
		// ZF:i: FM Index ops for seed alignment
		WRITE_SEP();
		itoa10<uint64_t>(prm.nSdFmops, buf);
		o.append("ZF:i:");
		o.append(buf);
		// Zf:i: FM Index ops for offset resolution
		WRITE_SEP();
		itoa10<uint64_t>(prm.nExFmops, buf);
		o.append("Zf:i:");
		o.append(buf);
	}
	if(print_zm_) {
		// ZM:Z: Print FM index op string for best-first search
		WRITE_SEP();
		o.append("ZM:Z:");
		prm.fmString.print(o, buf);
	}
	if(print_zi_) {
		// ZI:i: Seed extend loop iterations
		WRITE_SEP();
		itoa10<uint64_t>(prm.nExIters, buf);
		o.append("ZI:i:");
		o.append(buf);
	}
	if(print_xr_) {
		// Original read string
		o.append("\n");
		printOptFieldNewlineEscapedZ(o, rd.readOrigBuf);
	}
	if(print_zt_) {
		// ZT:Z: Extra features for MAPQ estimation
		
		// 1. AS:i for current mate
		// 2. diff or NA for current mate
		// 3. Like 1 for opposite mate
		// 4. Like 2 for opposite mate
		
		WRITE_SEP();
		const bool paired = flags.partOfPair();
		const TAlScore MN = std::numeric_limits<TAlScore>::min();
		TAlScore secondBest[2] = {MN, MN};
		TAlScore thirdBest[2] = {MN, MN};
		{
			for (int self = 0; self < (paired ? 2 : 1); self++) {
				// Second-best
				AlnScore sco;
				bool mate1 = rd.mate < 2;
				if(self > 0) mate1 = !mate1;
				if(flags.alignedConcordant()) {
					sco = summ.bestUnchosen(mate1);
				} else {
					sco = summ.secbest(mate1);
				}
				if(sco.valid()) {
					secondBest[self] = sco.score();
				}

				// Third-best
				thirdBest[self] = mate1 ? prm.bestLtMinscMate1 : prm.bestLtMinscMate2;
			}
		}
		TAlScore best[2] = {res.score().score(), res.oscore().score()};
		TAlScore diff[2] = {MN, MN};
		for(int self = 0; self < 2; self++) {
			const TAlScore mx = max(secondBest[self], thirdBest[self]);
			if(best[self] > MN && mx > MN) {
				diff[self] = best[self] - mx;
			}
		}
		TAlScore best_conc = MN, diff_conc = MN;
		if(paired && summ.bestPaired().valid()) {
			best_conc = summ.bestPaired().score();
			if(summ.secbestPaired().valid()) {
				diff_conc = best_conc - summ.secbestPaired().score();
			}
		}
		o.append("ZT:Z:");
		// AS:i for current mate
		itoa10<TAlScore>((int)best[0], buf);
		o.append(buf);
		o.append(",");
		// diff for current mate
		if(diff[0] > MN) {
			itoa10<TAlScore>((int)diff[0], buf);
			o.append(buf);
		} else {
			o.append("NA");
		}
		o.append(",");
		// AS:i for other mate
		if(best[1] > MN) {
			itoa10<TAlScore>((int)best[1], buf);
			o.append(buf);
		} else {
			o.append("NA");
		}
		o.append(",");
		// diff for other mate
		if(diff[1] > MN) {
			itoa10<TAlScore>((int)diff[1], buf);
			o.append(buf);
		} else {
			o.append("NA");
		}
		o.append(",");
		// Sum of AS:i for aligned pairs
		if(best_conc > MN) {
			itoa10<TAlScore>((int)best_conc, buf);
			o.append(buf);
		} else {
			o.append("NA");
		}
		o.append(",");
		// Diff for aligned pairs
		if(diff_conc > MN) {
			itoa10<TAlScore>((int)diff_conc, buf);
			o.append(buf);
		} else {
			o.append("NA");
		}
		// Flags related to seed hits, specific to this mate but not to the
		// strand aligned to
		int mate = (rd.mate < 2 ? 0 : 1);
		o.append(",");
		itoa10<TAlScore>((int)((prm.seedsPerNucMS[2 * mate] + prm.seedsPerNucMS[2 * mate + 1]) * 1000), buf);
		o.append(buf);
		o.append(",");
		itoa10<TAlScore>((int)((prm.seedPctUniqueMS[2 * mate] + prm.seedPctUniqueMS[2 * mate + 1]) * 1000), buf);
		o.append(buf);
		o.append(",");
		itoa10<TAlScore>((int)((prm.seedPctRepMS[2 * mate] + prm.seedPctRepMS[2 * mate + 1]) * 1000), buf);
		o.append(buf);
		o.append(",");
		itoa10<TAlScore>((int)((prm.seedHitAvgMS[2 * mate] + prm.seedHitAvgMS[2 * mate + 1]) + 0.5f), buf);
		o.append(buf);
		// Flags related to seed hits again, but specific both to this mate and
		// to the strand aligned to
		int fw = res.fw() ? 0 : 1;
		o.append(",");
		itoa10<TAlScore>((int)(prm.seedsPerNucMS[2 * mate + fw] * 1000), buf);
		o.append(buf);
		o.append(",");
		itoa10<TAlScore>((int)(prm.seedPctUniqueMS[2 * mate + fw] * 1000), buf);
		o.append(buf);
		o.append(",");
		itoa10<TAlScore>((int)(prm.seedPctRepMS[2 * mate + fw] * 1000), buf);
		o.append(buf);
		o.append(",");
		itoa10<TAlScore>((int)(prm.seedHitAvgMS[2 * mate + fw] + 0.5f), buf);
		o.append(buf);
	}
}

/**
 * Print the optional flags to the given string.
 */
void SamConfig::printEmptyOptFlags(
	BTString& o,               // output buffer
	bool first,                // first opt flag printed is first overall?
	const Read& rd,            // read
	const AlnFlags& flags,     // alignment flags
	const AlnSetSumm& summ,    // summary of alignments for this read
	const SeedAlSumm& ssm,     // seed alignment summary
	const PerReadMetrics& prm, // per-read metrics
	const Scoring& sc)         // scoring scheme
	const
{
	char buf[1024];
	if(print_yn_) {
		// YN:i: Minimum valid score for this mate
		TAlScore mn = sc.scoreMin.f<TAlScore>(rd.length());
		itoa10<TAlScore>(mn, buf);
		WRITE_SEP();
		o.append("YN:i:");
		o.append(buf);
		// Yn:i: Perfect score for this mate
		TAlScore pe = sc.perfectScore(rd.length());
		itoa10<TAlScore>(pe, buf);
		WRITE_SEP();
		o.append("Yn:i:");
		o.append(buf);
	}
	if(print_zs_) {
		// ZS:i: Pseudo-random seed for read
		itoa10<uint32_t>(rd.seed, buf);
		WRITE_SEP();
		o.append("ZS:i:");
		o.append(buf);
	}
	if(print_yt_) {
		// YT:Z: String representing alignment type
		WRITE_SEP();
		flags.printYT(o);
	}
	if(print_yp_ && flags.partOfPair() && flags.canMax()) {
		// YP:i: Read was repetitive when aligned paired?
		WRITE_SEP();
		flags.printYP(o);
	}
	if(print_ym_ && flags.canMax() && (flags.isMixedMode() || !flags.partOfPair())) {
		// YM:i: Read was repetitive when aligned unpaired?
		WRITE_SEP();
		flags.printYM(o);
	}
	if(print_yf_ && flags.filtered()) {
		// YF:i: Why read was filtered out prior to alignment
		first = flags.printYF(o, first) && first;
	}
	if(!rgs_.empty()) {
		WRITE_SEP();
		o.append(rgs_.c_str());
	}
	if(print_xt_) {
		// XT:i: Timing
		WRITE_SEP();
		struct timeval  tv_end;
		struct timezone tz_end;
		gettimeofday(&tv_end, &tz_end);
		size_t total_usecs =
			(tv_end.tv_sec  - prm.tv_beg.tv_sec) * 1000000 +
			(tv_end.tv_usec - prm.tv_beg.tv_usec);
		itoa10<size_t>(total_usecs, buf);
		o.append("XT:i:");
		o.append(buf);
	}
	if(print_xd_) {
		// XD:i: Extend DPs
		WRITE_SEP();
		itoa10<uint64_t>(prm.nExDps, buf);
		o.append("XD:i:");
		o.append(buf);
		// Xd:i: Mate DPs
		WRITE_SEP();
		itoa10<uint64_t>(prm.nMateDps, buf);
		o.append("Xd:i:");
		o.append(buf);
	}
	if(print_xu_) {
		// XU:i: Extend ungapped tries
		WRITE_SEP();
		itoa10<uint64_t>(prm.nExUgs, buf);
		o.append("XU:i:");
		o.append(buf);
		// Xu:i: Mate ungapped tries
		WRITE_SEP();
		itoa10<uint64_t>(prm.nMateUgs, buf);
		o.append("Xu:i:");
		o.append(buf);
	}
	if(print_ye_) {
		// YE:i: Streak of failed DPs at end
		WRITE_SEP();
		itoa10<uint64_t>(prm.nDpFail, buf);
		o.append("YE:i:");
		o.append(buf);
		// Ye:i: Streak of failed ungaps at end
		WRITE_SEP();
		itoa10<uint64_t>(prm.nUgFail, buf);
		o.append("Ye:i:");
		o.append(buf);
	}
	if(print_yl_) {
		// YL:i: Longest streak of failed DPs
		WRITE_SEP();
		itoa10<uint64_t>(prm.nDpFailStreak, buf);
		o.append("YL:i:");
		o.append(buf);
		// Yl:i: Longest streak of failed ungaps
		WRITE_SEP();
		itoa10<uint64_t>(prm.nUgFailStreak, buf);
		o.append("Yl:i:");
		o.append(buf);
	}
	if(print_yu_) {
		// YU:i: Index of last succesful DP
		WRITE_SEP();
		itoa10<uint64_t>(prm.nDpLastSucc, buf);
		o.append("YU:i:");
		o.append(buf);
		// Yu:i: Index of last succesful DP
		WRITE_SEP();
		itoa10<uint64_t>(prm.nUgLastSucc, buf);
		o.append("Yu:i:");
		o.append(buf);
	}
	if(print_xp_) {
		// XP:Z: String describing seed hits
		WRITE_SEP();
		o.append("XP:B:I,");
		itoa10<uint64_t>(prm.nSeedElts, buf);
		o.append(buf);
		o.append(',');
		itoa10<uint64_t>(prm.nSeedEltsFw, buf);
		o.append(buf);
		o.append(',');
		itoa10<uint64_t>(prm.nSeedEltsRc, buf);
		o.append(buf);
		o.append(',');
		itoa10<uint64_t>(prm.seedMean, buf);
		o.append(buf);
		o.append(',');
		itoa10<uint64_t>(prm.seedMedian, buf);
		o.append(buf);
	}
	if(print_yr_) {
		// YR:i: Redundant seed hits
		WRITE_SEP();
		itoa10<uint64_t>(prm.nRedundants, buf);
		o.append("YR:i:");
		o.append(buf);
	}
	if(print_zb_) {
		// ZB:i: Ftab ops for seed alignment
		WRITE_SEP();
		itoa10<uint64_t>(prm.nFtabs, buf);
		o.append("ZB:i:");
		o.append(buf);
	}
	if(print_zr_) {
		// ZR:Z: Redundant path skips in seed alignment
		WRITE_SEP();
		o.append("ZR:Z:");
		itoa10<uint64_t>(prm.nRedSkip, buf); o.append(buf);
		o.append(',');
		itoa10<uint64_t>(prm.nRedFail, buf); o.append(buf);
		o.append(',');
		itoa10<uint64_t>(prm.nRedIns, buf); o.append(buf);
	}
	if(print_zf_) {
		// ZF:i: FM Index ops for seed alignment
		WRITE_SEP();
		itoa10<uint64_t>(prm.nSdFmops, buf);
		o.append("ZF:i:");
		o.append(buf);
		// Zf:i: FM Index ops for offset resolution
		WRITE_SEP();
		itoa10<uint64_t>(prm.nExFmops, buf);
		o.append("Zf:i:");
		o.append(buf);
	}
	if(print_zm_) {
		// ZM:Z: Print FM index op string for best-first search
		WRITE_SEP();
		o.append("ZM:Z:");
		prm.fmString.print(o, buf);
	}
	if(print_zi_) {
		// ZI:i: Seed extend loop iterations
		WRITE_SEP();
		itoa10<uint64_t>(prm.nExIters, buf);
		o.append("ZI:i:");
		o.append(buf);
	}
	if(print_xr_) {
		// Original read string
		o.append("\n");
		printOptFieldNewlineEscapedZ(o, rd.readOrigBuf);
	}
}

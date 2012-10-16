/*
 * Copyright 2011, Ben Langmead <blangmea@jhsph.edu>
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

#include <ctype.h>
#include "aligner_seed2.h"
#include "assert_helpers.h"
#include "bt2_idx.h"

/**
 * Drive the process of descending from all search roots.
 */
void DescentDriver::go(
    const Scoring& sc,    // scoring scheme
    const Ebwt& ebwtFw,   // forward index
    const Ebwt& ebwtBw,   // mirror index
    DescentMetrics& met)  // metrics
{
    // Convert DescentRoots to the initial Descents
    for(size_t i = 0; i < roots_.size(); i++) {
        size_t dfsz = df_.size();
        size_t pfsz = pf_.size();
        TDescentId id = df_.alloc();
        Edit e_null;
        assert(!e_null.inited());
        bool succ = df_[id].init(
            q_,        // query
            i,         // root and conf id
            sc,        // scoring scheme
            id,        // new Descent's id
            ebwtFw,    // forward index
            ebwtBw,    // mirror index
			re_,       // redundancy checker
            df_,       // Descent factory
            pf_,       // DescentPos factory
            roots_,    // DescentRoots
            confs_,    // DescentConfs
            heap_,     // heap
            alsink_,   // alignment sink
            met);      // metrics
        if(!succ) {
            // Reclaim memory we had used for this descent and its DescentPos info
            df_.resize(dfsz);
            pf_.resize(pfsz);
        }
    }
    // Advance until some stopping condition
    bool stop = heap_.empty();
    while(!stop) {
		// Pop off the highest-priority descent.  Note that some outgoing edges
		// might have since been explored, which could reduce the priority of
		// the descent once we .
        TDescentPair p = heap_.pop();
        df_[p.second].followBestOutgoing(
            q_,
            ebwtFw,
            ebwtBw,
            sc,
			re_,       // redundancy checker
            df_,       // Descent factory
            pf_,       // DescentPos factory
            roots_,
            confs_,
            heap_,
            alsink_,
            met);
        stop = heap_.empty();
    }
}

/**
 * Initialize a new descent branching from the given descent via the given
 * edit.  Return false if the Descent has no outgoing edges (and can
 * therefore have its memory freed), true otherwise.
 */
bool Descent::init(
    const DescentQuery& q,          // query
    TRootId rid,                    // root id
    const Scoring& sc,              // scoring scheme
    TReadOff al5pi,                 // offset from 5' of 1st aligned char
    TReadOff al5pf,                 // offset from 5' of last aligned char
    TIndexOff topf,                 // SA range top in FW index
    TIndexOff botf,                 // SA range bottom in FW index
    TIndexOff topb,                 // SA range top in BW index
    TIndexOff botb,                 // SA range bottom in BW index
    bool l2r,                       // direction this descent will go in
    size_t descid,                  // my ID
    TDescentId parent,              // parent ID
    TScore pen,                     // total penalties so far
    const Edit& e,                  // edit for incoming edge
    const Ebwt& ebwtFw,             // forward index
    const Ebwt& ebwtBw,             // mirror index
	DescentRedundancyChecker& re,   // redundancy checker
    EFactory<Descent>& df,          // Descent factory
    EFactory<DescentPos>& pf,       // DescentPos factory
    const EList<DescentRoot>& rs,   // roots
    const EList<DescentConfig>& cs, // configs
    EHeap<TDescentPair>& heap,      // heap
    DescentAlignmentSink& alsink,   // alignment sink
    DescentMetrics& met)            // metrics
{
    rid_ = rid;
    al5pi_ = al5pi;
    al5pf_ = al5pf;
    l2r_ = l2r;
    topf_ = topf;
    botf_ = botf;
    topb_ = topb;
    botb_ = botb;
    descid_ = descid;
    parent_ = parent;
    pen_ = pen;
    posid_ = std::numeric_limits<size_t>::max();
    len_ = 0;
    out_.clear();
    edit_ = e;
    lastRecalc_ = true;
	gapadd_ = df[parent].gapadd_;
	if(e.inited()) {
		if(e.isReadGap()) {
			gapadd_++;
		} else if(e.isRefGap()) {
			gapadd_--;
		}
	}
    bool branches = false, hitEnd = false, done = false;
    TIndexOff topf_new = 0, botf_new = 0, topb_new = 0, botb_new = 0;
    off5p_i_ = 0;
#ifndef NDEBUG
    size_t depth = al5pf_ - al5pi_ + 1;
    TScore maxpen = cs[rid_].cons[depth]; // maximum penalty total at this position
    assert_geq(maxpen, pen_);    // can't have already exceeded max penalty
#endif
    bool matchSucc = followMatches(
        q,
        ebwtFw,
        ebwtBw,
		re,
        df,
        pf,
        rs,
        cs,
        heap,
        alsink,
        met,
        branches,
        hitEnd,
        done,
        off5p_i_,
        topf_new,
        botf_new,
        topb_new,
        botb_new);
    bool bounceSucc = false;
    if(hitEnd && !done) {
        bounceSucc = bounce(
            q,
            topf_new,
            botf_new,
            topb_new,
            botb_new,
            ebwtFw,
            ebwtBw,
            sc,
			re,
            df,
            pf,
            rs,
            cs,
            heap,
            alsink,
            met);
    }
	if(matchSucc) {
		// Calculate info about outgoing edges
		recalcOutgoing(q, sc, re, pf, rs, cs);
		if(!empty()) {
			heap.insert(make_pair(out_.bestPri(), descid)); // Add to heap
		}
	}
    return !empty() || bounceSucc;
}

/**
 * Initialize a new descent beginning at the given root.  Return false if
 * the Descent has no outgoing edges (and can therefore have its memory
 * freed), true otherwise.
 */
bool Descent::init(
    const DescentQuery& q,          // query
    TRootId rid,                    // root id
    const Scoring& sc,              // scoring scheme
    size_t descid,                  // id of this Descent
    const Ebwt& ebwtFw,             // forward index
    const Ebwt& ebwtBw,             // mirror index
	DescentRedundancyChecker& re,   // redundancy checker
    EFactory<Descent>& df,          // Descent factory
    EFactory<DescentPos>& pf,       // DescentPos factory
    const EList<DescentRoot>& rs,   // roots
    const EList<DescentConfig>& cs, // configs
    EHeap<TDescentPair>& heap,      // heap
    DescentAlignmentSink& alsink,   // alignment sink
    DescentMetrics& met)            // metrics
{
    rid_ = rid;
    al5pi_ = rs[rid].off5p;
    al5pf_ = rs[rid].off5p;
    l2r_ = rs[rid].l2r;
    topf_ = botf_ = topb_ = botb_ = 0;
    descid_ = descid;
    parent_ = std::numeric_limits<size_t>::max();
    pen_ = 0;
    posid_ = std::numeric_limits<size_t>::max();
    len_ = 0;
    out_.clear();
    edit_.reset();
    lastRecalc_ = true;
	gapadd_ = 0;
    bool branches = false, hitEnd = false, done = false;
    TIndexOff topf_new = 0, botf_new = 0, topb_new = 0, botb_new = 0;
    off5p_i_ = 0;
    bool matchSucc = followMatches(
        q,
        ebwtFw,
        ebwtBw,
		re,
        df,
        pf,
        rs,
        cs,
        heap,
        alsink,
        met,
        branches,
        hitEnd,
        done,
        off5p_i_,
        topf_new,
        botf_new,
        topb_new,
        botb_new);
    bool bounceSucc = false;
    if(hitEnd && !done) {
        bounceSucc = bounce(
            q,
            topf_new,
            botf_new,
            topb_new,
            botb_new,
            ebwtFw,
            ebwtBw,
            sc,
			re,
            df,
            pf,
            rs,
            cs,
            heap,
            alsink,
            met);
    }
    // Calculate info about outgoing edges
    assert(empty());
	if(matchSucc) {
		recalcOutgoing(q, sc, re, pf, rs, cs);
		if(!empty()) {
			heap.insert(make_pair(out_.bestPri(), descid)); // Add to heap
		}
	}
    return !empty() || bounceSucc;
}

/**
 * Recalculate our summary of the outgoing edges from this descent.  When
 * deciding what outgoing edges are legal, we abide by constraints.
 * Typically, they limit the total of the penalties accumulated so far, as
 * a function of distance from the search root.  E.g. a constraint might
 * disallow any gaps or mismatches within 20 ply of the search root, then
 * allow 1 mismatch within 30 ply, then allow up to 1 mismatch or 1 gap
 * within 40 ply, etc.
 *
 * Return the total number of valid outgoing edges found.
 *
 * TODO: Eliminate outgoing gap edges that are redundant with others owing to
 *       the DNA sequence and the fact that we don't care to distinguish among
 *       "equivalent" homopolymer extensinos and retractions.
 */
size_t Descent::recalcOutgoing(
    const DescentQuery& q,           // query string
    const Scoring& sc,               // scoring scheme
	DescentRedundancyChecker& re,    // redundancy checker
    EFactory<DescentPos>& pf,        // factory with DescentPoss
    const EList<DescentRoot>& rs,    // roots
    const EList<DescentConfig>& cs)  // configs
{
    assert_eq(botf_ - topf_, botb_ - topb_);
	assert(out_.empty());
	assert(repOk(&q));
	// Get initial 5' and 3' offsets
    bool fw = rs[rid_].fw;
    float rootpri = rs[rid_].pri;
	bool toward3p = (l2r_ == fw);
	size_t off5p = off5p_i_;
	assert_geq(al5pf_, al5pi_);
	size_t off3p = q.length() - off5p - 1;
	// By "depth" we essentially mean the number of characters already aligned
	size_t depth, extrai = 0, extraf = 0;
	size_t cur5pi = al5pi_, cur5pf = al5pf_;
    if(toward3p) {
		// Toward 3'
		cur5pf = off5p;
        depth = off5p - al5pi_;
		// Failed to match out to the end?
		if(al5pf_ < q.length() - 1) {
			extraf = 1; // extra 
		}
    } else {
		// Toward 5'
		cur5pi = off5p;
        depth = al5pf_ - off5p;
		if(al5pi_ > 0) {
			extrai = 1;
		}
    }
	// Get gap penalties
	TScore pen_rdg_ex = sc.readGapExtend(), pen_rfg_ex = sc.refGapExtend();
	TScore pen_rdg_op = sc.readGapOpen(),   pen_rfg_op = sc.refGapOpen();
	// Top and bot in the direction of the descent
	TIndexOff top  = l2r_ ? topb_ : topf_;
	TIndexOff bot  = l2r_ ? botb_ : botf_;
	// Top and bot in the opposite direction
	TIndexOff topp = l2r_ ? topf_ : topb_;
	TIndexOff botp = l2r_ ? botf_ : botb_;
	assert_eq(botp - topp, bot - top);
	DescentEdge edge;
	size_t nout = 0;
	// Enumerate all outgoing edges, starting at the root and going out
    size_t d = posid_;
	// At first glance, we might think we should be bounded by al5pi_ and
	// al5pf_, but those delimit the positions that matched between reference
	// and read.  If we hit a position that failed to match as part of
	// followMatches, then we also want to evaluate ways of leaving that
	// position, which adds one more position to viist.
	while(off5p >= al5pi_ - extrai && off5p <= al5pf_ + extraf) {
        assert_lt(off5p, q.length());
        assert_lt(off3p, q.length());
		TScore maxpen = cs[rid_].cons[depth]; // maximum penalty total at this position
		assert_geq(maxpen, pen_);    // can't have already exceeded max penalty
		TScore diff = maxpen - pen_; // room we have left
		// Get pointer to SA ranges in the direction of descent
		const TIndexOff *t  = l2r_ ? pf[d].topb : pf[d].topf;
		const TIndexOff *b  = l2r_ ? pf[d].botb : pf[d].botf;
		const TIndexOff *tp = l2r_ ? pf[d].topf : pf[d].topb;
		const TIndexOff *bp = l2r_ ? pf[d].botf : pf[d].botb;
		assert_eq(pf[d].botf - pf[d].topf, pf[d].botb - pf[d].topb);
		// What are the read char / quality?
		std::pair<int, int> p = q.get(off5p, fw);
		int c = p.first;
		assert_range(0, 4, c);
		// Only entertain edits if there is at least one type of edit left and
		// there is some penalty budget left
		if(!pf[d].flags.exhausted() && diff > 0) {
			// What would the penalty be if we mismatched at this position?
			// This includes the case where the mismatch is for an N in the
			// read.
			int qq = p.second;
            assert_geq(qq, 0);
			TScore pen_mm = sc.mm(c, qq);
			if(pen_mm <= diff) {
				for(int j = 0; j < 4; j++) {
					if(j == c) continue; // Match, not mismatch
					if(b[j] <= t[j]) {
						continue; // No outgoing edge with this nucleotide
					}
					if(!pf[d].flags.mmExplore(j)) {
						continue; // Already been explored
					}
					TIndexOff topf = pf[d].topf[j], botf = pf[d].botf[j];
					TIndexOff topb = pf[d].topb[j], botb = pf[d].botb[j];
					if(re.contains(fw, cur5pi, cur5pf, cur5pf - cur5pi + 1 + gapadd_, topf, botf, pen_ + pen_mm)) {
						continue; // Redundant with a path already explored
					}
					TIndexOff width = b[j] - t[j];
					Edit edit((uint32_t)off5p, (int)("ACGTN"[j]), (int)("ACGTN"[c]), EDIT_TYPE_MM);
					DescentPriority pri(pen_ + pen_mm, depth, width, rootpri);
                    assert(topf != 0 || botf != 0);
                    assert(topb != 0 || botb != 0);
					assert_eq(botb - topb, botf - topf);
					edge.init(edit, off5p, pri, d
#ifndef NDEBUG
                    , d, topf, botf, topb, botb
#endif
                    );
					out_.update(edge);
					nout++;
				}
			}
			bool gapsAllowed = (off5p >= sc.gapbar && off3p >= sc.gapbar);
			if(gapsAllowed) {
				// An easy redundancy check is: if all ways of proceeding are
				// matches, then there's no need to entertain gaps here.
				// Shifting the gap one position further downstream is
				// guarnteed not to be worse.
				size_t totwidth = (b[0] - t[0]) +
				                  (b[1] - t[1]) +
								  (b[2] - t[2]) +
								  (b[3] - t[3]);
				assert_leq(b[c] - t[c], totwidth);
				bool allmatch = (totwidth == (b[c] - t[c]));
				bool rdex = false, rfex = false;
				size_t cur5pi_i = cur5pi, cur5pf_i = cur5pf;
				if(toward3p) {
					cur5pf_i--;
				} else {
					cur5pi_i++;
				}
				if(off5p == off5p_i_ && edit_.inited()) {
					// If we're at the root of the descent, and the descent
					// branched on a gap, then this could be scored as an
					// extension of that gap.
					if(pen_rdg_ex <= diff && edit_.isReadGap()) {
						// Extension of a read gap
						rdex = true;
						for(int j = 0; j < 4; j++) {
							if(b[j] <= t[j]) {
								continue; // No outgoing edge with this nucleotide
							}
							if(!pf[d].flags.rdgExplore(j)) {
								continue; // Already been explored
							}
							TIndexOff topf = pf[d].topf[j], botf = pf[d].botf[j];
							TIndexOff topb = pf[d].topb[j], botb = pf[d].botb[j];
							assert(topf != 0 || botf != 0);
							assert(topb != 0 || botb != 0);
							if(re.contains(fw, cur5pi_i, cur5pf_i, cur5pf - cur5pi + 1 + gapadd_, topf, botf, pen_ + pen_rdg_ex)) {
								continue; // Redundant with a path already explored
							}
							TIndexOff width = b[j] - t[j];
							uint32_t off = (uint32_t)off5p + (l2r_ ? 0 : 1);
							Edit edit(off, (int)("ACGTN"[j]), '-', EDIT_TYPE_READ_GAP);
							edit.pos2 = edit_.pos2 + (l2r() ? 1 : -1);
							DescentPriority pri(pen_ + pen_rdg_ex, depth, width, rootpri);
                            assert(topf != 0 || botf != 0);
                            assert(topb != 0 || botb != 0);
							assert_eq(botb - topb, botf - topf);
							edge.init(edit, off5p, pri, d
#ifndef NDEBUG
                            , d,
                            topf, botf, topb, botb
#endif
                            );
							out_.update(edge);
							nout++;
						}
					}
					if(pen_rfg_ex <= diff && edit_.isRefGap()) {
						// Extension of a reference gap
						rfex = true;
						if(pf[d].flags.rfgExplore()) {
                            TIndexOff topf = l2r_ ? topp : top;
                            TIndexOff botf = l2r_ ? botp : bot;
							TIndexOff topb = l2r_ ? top : topp;
							TIndexOff botb = l2r_ ? bot : botp;
							assert(topf != 0 || botf != 0);
							assert(topb != 0 || botb != 0);
							size_t nrefal = cur5pf - cur5pi + gapadd_;
							if(!re.contains(fw, cur5pi, cur5pf, nrefal, topf, botf, pen_ + pen_rfg_ex)) {
								TIndexOff width = bot - top;
								Edit edit((uint32_t)off5p, '-', (int)("ACGTN"[c]), EDIT_TYPE_REF_GAP);
								DescentPriority pri(pen_ + pen_rfg_ex, depth, width, rootpri);
								assert(topf != 0 || botf != 0);
								assert(topb != 0 || botb != 0);
								edge.init(edit, off5p, pri, d
#ifndef NDEBUG
								// It's a little unclear what the depth ought to be.
								// Is it the depth we were at when we did the ref
								// gap?  I.e. the depth of the flags where rfgExplore()
								// returned true?  Or is it the depth where we can
								// retrieve the appropriate top/bot?  We make it the
								// latter, might wrap around, indicating we should get
								// top/bot from the descent's topf_, ... fields.
								, (d == posid_) ? std::numeric_limits<size_t>::max() : (d - 1),
								topf, botf, topb, botb
#endif
								);
								out_.update(edge);
								nout++;
							}
						}
					}
				}
				if(!allmatch && pen_rdg_op <= diff && !rdex) {
					// Opening a new read gap
					for(int j = 0; j < 4; j++) {
						if(b[j] <= t[j]) {
							continue; // No outgoing edge with this nucleotide
						}
						if(!pf[d].flags.rdgExplore(j)) {
							continue; // Already been explored
						}
						TIndexOff topf = pf[d].topf[j], botf = pf[d].botf[j];
						TIndexOff topb = pf[d].topb[j], botb = pf[d].botb[j];
						assert(topf != 0 || botf != 0);
						assert(topb != 0 || botb != 0);
						if(re.contains(fw, cur5pi_i, cur5pf_i, cur5pf - cur5pi + 1 + gapadd_, topf, botf, pen_ + pen_rdg_op)) {
							continue; // Redundant with a path already explored
						}
						TIndexOff width = b[j] - t[j];
						uint32_t off = (uint32_t)off5p + (l2r_ ? 0 : 1);
						Edit edit(off, (int)("ACGTN"[j]), '-', EDIT_TYPE_READ_GAP);
						DescentPriority pri(pen_ + pen_rdg_op, depth, width, rootpri);
                        assert(topf != 0 || botf != 0);
                        assert(topb != 0 || botb != 0);
						assert_eq(botb - topb, botf - topf);
						edge.init(edit, off5p, pri, d
#ifndef NDEBUG
                        , d, topf, botf, topb, botb
#endif
                        );
						out_.update(edge);
						nout++;
					}
				}
				if(!allmatch && pen_rfg_op <= diff && !rfex) {
					// Opening a new reference gap
                    if(pf[d].flags.rfgExplore()) {
                        TIndexOff topf = l2r_ ? topp : top;
                        TIndexOff botf = l2r_ ? botp : bot;
						TIndexOff topb = l2r_ ? top : topp;
						TIndexOff botb = l2r_ ? bot : botp;
						assert(topf != 0 || botf != 0);
						assert(topb != 0 || botb != 0);
						size_t nrefal = cur5pf - cur5pi + gapadd_;
						if(!re.contains(fw, cur5pi, cur5pf, nrefal, topf, botf, pen_ + pen_rfg_op)) {
							TIndexOff width = bot - top;
							Edit edit((uint32_t)off5p, '-', (int)("ACGTN"[c]), EDIT_TYPE_REF_GAP);
							DescentPriority pri(pen_ + pen_rfg_op, depth, width, rootpri);
							assert(topf != 0 || botf != 0);
							assert(topb != 0 || botb != 0);
							edge.init(edit, off5p, pri, d
#ifndef NDEBUG
							// It's a little unclear what the depth ought to be.
							// Is it the depth we were at when we did the ref
							// gap?  I.e. the depth of the flags where rfgExplore()
							// returned true?  Or is it the depth where we can
							// retrieve the appropriate top/bot?  We make it the
							// latter, might wrap around, indicating we should get
							// top/bot from the descent's topf_, ... fields.
							, (d == posid_) ? std::numeric_limits<size_t>::max() : (d - 1),
							topf, botf, topb, botb
#endif
							);
							out_.update(edge);
							nout++;
						}
                    }
				}
			}
		}
		// Update off5p, off3p, depth
        d++;
		depth++;
        assert_leq(depth, al5pf_ - al5pi_ + 2);
        if(toward3p) {
            if(off3p == 0) {
                break;
            }
            off5p++;
            off3p--;
			cur5pf++;
        } else {
            if(off5p == 0) {
                break;
            }
            off3p++;
            off5p--;
			cur5pi--;
        }
		// Update top and bot
		top = t[c], topp = tp[c];
		bot = b[c], botp = bp[c];
		assert_eq(bot-top, botp-topp);
	}
	lastRecalc_ = (nout <= 5);
    out_.best1.updateFlags(pf);
    out_.best2.updateFlags(pf);
    out_.best3.updateFlags(pf);
    out_.best4.updateFlags(pf);
    out_.best5.updateFlags(pf);
	return nout;
}

void Descent::print(
	std::ostream& os,
	const char *prefix,
	const DescentQuery& q,
	size_t trimLf,
	size_t trimRg,
	bool fw,
	const EList<Edit>& edits,
	size_t ei,
	size_t en,
	BTDnaString& rf) const
{
	const BTDnaString& read = fw ? *q.seq : *q.seqrc;
	size_t eidx = ei;
	os << prefix;
	// Print read
	for(size_t i = 0; i < read.length(); i++) {
		if(i < trimLf || i >= read.length() - trimRg) {
			os << (char)tolower(read.toChar(i));
			continue;
		}
		bool ins = false, del = false, mm = false;
		while(eidx < ei + en && edits[eidx].pos == i) {
			if(edits[eidx].isReadGap()) {
				ins = true;
				os << '-';
			} else if(edits[eidx].isRefGap()) {
				del = true;
				assert_eq((int)edits[eidx].qchr, read.toChar(i));
				os << read.toChar(i);
			} else {
				mm = true;
				assert(edits[eidx].isMismatch());
				assert_eq((int)edits[eidx].qchr, read.toChar(i));
				os << (char)edits[eidx].qchr;
			}
			eidx++;
		}
		if(!del && !mm) {
			// Print read character
			os << read.toChar(i);
		}
	}
	os << endl;
	os << prefix;
	eidx = ei;
	// Print match bars
	for(size_t i = 0; i < read.length(); i++) {
		if(i < trimLf || i >= read.length() - trimRg) {
			os << ' ';
			continue;
		}
		bool ins = false, del = false, mm = false;
		while(eidx < ei + en && edits[eidx].pos == i) {
			if(edits[eidx].isReadGap()) {
				ins = true;
				os << ' ';
			} else if(edits[eidx].isRefGap()) {
				del = true;
				os << ' ';
			} else {
				mm = true;
				assert(edits[eidx].isMismatch());
				os << ' ';
			}
			eidx++;
		}
		if(!del && !mm) os << '|';
	}
	os << endl;
	os << prefix;
	eidx = ei;
	// Print reference
	for(size_t i = 0; i < read.length(); i++) {
		if(i < trimLf || i >= read.length() - trimRg) {
			os << ' ';
			continue;
		}
		bool ins = false, del = false, mm = false;
		while(eidx < ei + en && edits[eidx].pos == i) {
			if(edits[eidx].isReadGap()) {
				ins = true;
				rf.appendChar((char)edits[eidx].chr);
				os << (char)edits[eidx].chr;
			} else if(edits[eidx].isRefGap()) {
				del = true;
				os << '-';
			} else {
				mm = true;
				assert(edits[eidx].isMismatch());
				rf.appendChar((char)edits[eidx].chr);
				os << (char)edits[eidx].chr;
			}
			eidx++;
		}
		if(!del && !mm) {
			rf.append(read[i]);
			os << read.toChar(i);
		}
	}
	os << endl;
}

/**
 * Create a new Descent 
 */
bool Descent::bounce(
	const DescentQuery& q,          // query string
    TIndexOff topf,                 // SA range top in fw index
    TIndexOff botf,                 // SA range bottom in fw index
    TIndexOff topb,                 // SA range top in bw index
    TIndexOff botb,                 // SA range bottom in bw index
	const Ebwt& ebwtFw,             // forward index
	const Ebwt& ebwtBw,             // mirror index
	const Scoring& sc,              // scoring scheme
	DescentRedundancyChecker& re,   // redundancy checker
	EFactory<Descent>& df,          // factory with Descent
	EFactory<DescentPos>& pf,       // factory with DescentPoss
    const EList<DescentRoot>& rs,   // roots
    const EList<DescentConfig>& cs, // configs
	EHeap<TDescentPair>& heap,      // heap of descents
    DescentAlignmentSink& alsink,   // alignment sink
    DescentMetrics& met)            // metrics
{
    assert_gt(botf, topf);
    assert(al5pi_ == 0 || al5pf_ == q.length()-1);
    assert(!(al5pi_ == 0 && al5pf_ == q.length()-1));
    size_t dfsz = df.size();
    size_t pfsz = pf.size();
	TDescentId id = df.alloc();
    Edit e_null;
    assert(!e_null.inited());
	// Follow matches 
	bool succ = df[id].init(
		q,         // query
        rid_,      // root id
		sc,        // scoring scheme
		al5pi_,    // new near-5' extreme
		al5pf_,    // new far-5' extreme
		topf,      // SA range top in FW index
		botf,      // SA range bottom in FW index
		topb,      // SA range top in BW index
		botb,      // SA range bottom in BW index
		!l2r_,     // direction this descent will go in; opposite from parent
		id,        // my ID
		descid_,   // parent ID
		pen_,      // total penalties so far - same as parent
		e_null,    // edit for incoming edge; uninitialized if bounced
		ebwtFw,    // forward index
		ebwtBw,    // mirror index
		re,        // redundancy checker
		df,        // Descent factory
		pf,        // DescentPos factory
        rs,        // DescentRoot list
        cs,        // DescentConfig list
		heap,      // heap
        alsink,    // alignment sink
		met);      // metrics
    if(!succ) {
        // Reclaim memory we had used for this descent and its DescentPos info
        df.resize(dfsz);
        pf.resize(pfsz);
    }
    return succ;
}

/**
 * Take the best outgoing edge and place it in the heap.  When deciding what
 * outgoing edges exist, abide by constraints in DescentConfig.  These
 * constraints limit total penalty accumulated so far versus distance from
 * search root.  E.g. a constraint might disallow any gaps or mismatches within
 * 20 ply of the root, then allow 1 mismatch within 30 ply, 1 mismatch or 1 gap
 * within 40 ply, etc.
 */
void Descent::followBestOutgoing(
	const DescentQuery& q,          // query string
	const Ebwt& ebwtFw,             // forward index
	const Ebwt& ebwtBw,             // mirror index
	const Scoring& sc,              // scoring scheme
	DescentRedundancyChecker& re,   // redundancy checker
	EFactory<Descent>& df,          // factory with Descent
	EFactory<DescentPos>& pf,       // factory with DescentPoss
    const EList<DescentRoot>& rs,   // roots
    const EList<DescentConfig>& cs, // configs
	EHeap<TDescentPair>& heap,      // heap of descents
    DescentAlignmentSink& alsink,   // alignment sink
	DescentMetrics& met)            // metrics
{
	// We assume this descent has been popped off the heap.  We'll re-add it if
	// it hasn't been exhausted.
	assert(!empty());
	assert(!out_.empty());
	while(!out_.empty()) {
		DescentPriority best = out_.bestPri();
		DescentEdge e = out_.rotate();
		TReadOff al5pi_new = al5pi_, al5pf_new = al5pf_;
		bool fw = rs[rid_].fw;
		bool toward3p = (l2r_ == fw);
		TReadOff edoff = e.off5p; // 5' offset of edit
		assert_leq(edoff, al5pf_ + 1);
		assert_geq(edoff + 1, al5pi_);
		if(out_.empty()) {
			if(!lastRecalc_) {
				recalcOutgoing(q, sc, re, pf, rs, cs);
				if(empty()) {
					// Could happen, since some outgoing edges may have become
					// redundant in the meantime.
					break;
				}
			} else {
				assert(empty());
			}
		}
		TReadOff doff; // edit's offset into this descent
		int chr = asc2dna[e.e.chr];
		// hitEnd is set to true iff this edit pushes us to the extreme 5' or 3'
		// end of the alignment
		bool hitEnd = false;
		// done is set to true iff this edit aligns the only remaining character of
		// the read
		bool done = false;
		if(toward3p) {
			// The 3' extreme of the new Descent is further in (away from the 3'
			// end) than the parent's.
			al5pf_new = doff = edoff;
			if(e.e.isReadGap()) {
				// We didn't actually consume the read character at 'edoff', so
				// retract al5pf_new by one position.  This doesn't effect the
				// "depth" (doff) of the SA range we took, though.
				assert_gt(al5pf_new, 0);
				al5pf_new--;
			}
			assert_lt(al5pf_new, q.length());
			hitEnd = (al5pf_new == q.length() - 1);
			done = (hitEnd && al5pi_new == 0);
			assert_geq(doff, off5p_i_);
			doff = doff - off5p_i_;
			assert_leq(doff, len_);
		} else {
			// The 5' extreme of the new Descent is further in (away from the 5'
			// end) than the parent's.
			al5pi_new = doff = edoff;
			if(e.e.isReadGap()) {
				// We didn't actually consume the read character at 'edoff', so
				// move al5pi_new closer to the 3' end by one position.  This
				// doesn't effect the "depth" (doff) of the SA range we took,
				// though.
				al5pi_new++;
			}
			hitEnd = (al5pi_new == 0);
			done = (hitEnd && al5pf_new == q.length() - 1);
			assert_geq(off5p_i_, doff);
			doff = off5p_i_ - doff;
			assert_leq(doff, len_);
		}
		// Check if this is redundant with an already-explored path
		// TODO: This *can't* be right, because it prevents us from trying 2
		// different outgoing mismatch edges from the same descent
		//if(!re.check(al5pi_new, al5pf_new, topf_, botf_, topb_, botb_, best.pen)) {
		//	continue; // try again
		//}
		bool l2r = l2r_; // gets overridden if we bounce
		if(!done && hitEnd) {
			// Alignment finsihed extending in one direction
			l2r = !l2r;
		}
		size_t dfsz = df.size();
		size_t pfsz = pf.size();
		TDescentId id = df.alloc();
		TIndexOff topf, botf, topb, botb;
		size_t d = posid_ + doff;
		if(e.e.isRefGap()) {
			d--; // might underflow
			if(doff == 0) {
				topf = topf_;
				botf = botf_;
				topb = topb_;
				botb = botb_;
				d = std::numeric_limits<size_t>::max();
				assert_eq(botf-topf, botb-topb);
			} else {
				assert_gt(al5pf_new, 0);
				assert_gt(d, 0);
				chr = pf[d].c;
				assert(pf[d].inited());
				assert_range(0, 3, chr);
				topf = pf[d].topf[chr];
				botf = pf[d].botf[chr];
				topb = pf[d].topb[chr];
				botb = pf[d].botb[chr];
				assert_eq(botf-topf, botb-topb);
			}
		} else {
			// A read gap or a mismatch
			assert(pf[d].inited());
			topf = pf[d].topf[chr];
			botf = pf[d].botf[chr];
			topb = pf[d].topb[chr];
			botb = pf[d].botb[chr];
			assert_eq(botf-topf, botb-topb);
		}
		assert_eq(d, e.d);
		assert_eq(topf, e.topf);
		assert_eq(botf, e.botf);
		assert_eq(topb, e.topb);
		assert_eq(botb, e.botb);
		if(done) {
			// Aligned the entire read end-to-end.  Presumably there's no need to
			// create a new Descent object.  We just report the alignment.
			alsink.reportAlignment(
				q,        // query
				ebwtFw,   // forward index
				ebwtBw,   // backward index
				topf,     // top of SA range in forward index
				botf,     // bottom of SA range in forward index
				topb,     // top of SA range in backward index
				botb,     // bottom of SA range in backward index
				descid_,  // Descent at the leaf
				rid_,     // root id
				e.e,      // extra edit, if necessary
				best.pen, // penalty
				df,       // factory with Descent
				pf,       // factory with DescentPoss
				rs,       // roots
				cs);      // configs
			return;
		}
		assert(al5pi_new != 0 || al5pf_new != q.length() - 1);
		bool succ = df[id].init(
			q,         // query
			rid_,      // root id
			sc,        // scoring scheme
			al5pi_new, // new near-5' extreme
			al5pf_new, // new far-5' extreme
			topf,      // SA range top in FW index
			botf,      // SA range bottom in FW index
			topb,      // SA range top in BW index
			botb,      // SA range bottom in BW index
			l2r,       // direction this descent will go in
			id,        // my ID
			descid_,   // parent ID
			best.pen,  // total penalties so far
			e.e,       // edit for incoming edge; uninitialized if bounced
			ebwtFw,    // forward index
			ebwtBw,    // mirror index
			re,        // redundancy checker
			df,        // Descent factory
			pf,        // DescentPos factory
			rs,        // DescentRoot list
			cs,        // DescentConfig list
			heap,      // heap
			alsink,    // alignment sink
			met);      // metrics
		if(!succ) {
			// Reclaim memory we had used for this descent and its DescentPos info
			df.resize(dfsz);
			pf.resize(pfsz);
		}
		break;
	}
	if(!empty()) {
		// Re-insert this Descent with its new priority
		heap.insert(make_pair(out_.bestPri(), descid_));
	}
}

/**
 * Given the forward and backward indexes, and given topf/botf/topb/botb, get
 * tloc, bloc ready for the next step.
 */
void Descent::nextLocsBi(
	const Ebwt& ebwtFw, // forward index
	const Ebwt& ebwtBw, // mirror index
	SideLocus& tloc,    // top locus
	SideLocus& bloc,    // bot locus
	TIndexOff topf,     // top in BWT
	TIndexOff botf,     // bot in BWT
	TIndexOff topb,     // top in BWT'
	TIndexOff botb)     // bot in BWT'
{
	assert_gt(botf, 0);
	// Which direction are we going in next?
	if(l2r_) {
		// Left to right; use BWT'
		if(botb - topb == 1) {
			// Already down to 1 row; just init top locus
			tloc.initFromRow(topb, ebwtBw.eh(), ebwtBw.ebwt());
			bloc.invalidate();
		} else {
			SideLocus::initFromTopBot(
				topb, botb, ebwtBw.eh(), ebwtBw.ebwt(), tloc, bloc);
			assert(bloc.valid());
		}
	} else {
		// Right to left; use BWT
		if(botf - topf == 1) {
			// Already down to 1 row; just init top locus
			tloc.initFromRow(topf, ebwtFw.eh(), ebwtFw.ebwt());
			bloc.invalidate();
		} else {
			SideLocus::initFromTopBot(
				topf, botf, ebwtFw.eh(), ebwtFw.ebwt(), tloc, bloc);
			assert(bloc.valid());
		}
	}
	// Check if we should update the tracker with this refinement
	assert(botf - topf == 1 ||  bloc.valid());
	assert(botf - topf > 1  || !bloc.valid());
}

/**
 * Advance this descent by following read matches as far as possible.
 *
 * This routine doesn't have to consider the whole gamut of constraints on
 * which outgoing edges can be followed.  If it is a root descent, it does have
 * to know how deep the no-edit constraint goes, though, so we can decide
 * whether using the ftab would potentially jump over relevant branch points.
 * Apart from that, though, we simply proceed as far as it can go by matching
 * characters in the query, irrespective of the constraints.
 * recalcOutgoing(...) and followBestOutgoing(...) do have to consider these
 * constraints, though.
 *
 * Conceptually, as we make descending steps, we have:
 * 1. Before each step, a single range indicating how we departed the previous
 *    step
 * 2. As part of each step, a quad of ranges indicating what range would result
 *    if we proceeded on an a, c, g ot t
 *
 * Return true iff it is possible to branch from this descent.  If we haven't
 * exceeded the no-branch depth.
 */
bool Descent::followMatches(
	const DescentQuery& q,     // query string
	const Ebwt& ebwtFw,        // forward index
	const Ebwt& ebwtBw,        // mirror index
	DescentRedundancyChecker& re, // redundancy checker
	EFactory<Descent>& df,     // Descent factory
	EFactory<DescentPos>& pf,  // DescentPos factory
    const EList<DescentRoot>& rs,   // roots
    const EList<DescentConfig>& cs, // configs
	EHeap<TDescentPair>& heap, // heap
    DescentAlignmentSink& alsink, // alignment sink
	DescentMetrics& met,       // metrics
    bool& branches,            // out: true -> there are > 0 ways to branch
    bool& hitEnd,              // out: true -> hit read end with non-empty range
    bool& done,                // out: true -> we made a full alignment
    TReadOff& off5p_i,         // out: initial 5' offset
    TIndexOff& topf_bounce,    // out: top of SA range for fw idx for bounce
    TIndexOff& botf_bounce,    // out: bot of SA range for fw idx for bounce
    TIndexOff& topb_bounce,    // out: top of SA range for bw idx for bounce
    TIndexOff& botb_bounce)    // out: bot of SA range for bw idx for bounce
{
	size_t nobranchDepth = 20;
	assert(repOk(&q));
	assert_eq(ebwtFw.eh().ftabChars(), ebwtBw.eh().ftabChars());
#ifndef NDEBUG
	for(int i = 0; i < 4; i++) {
		assert_eq(ebwtFw.fchr()[i], ebwtBw.fchr()[i]);
	}
#endif
	SideLocus tloc, bloc;
	TIndexOff topf = topf_, botf = botf_, topb = topb_, botb = botb_;
    bool fw = rs[rid_].fw;
	bool toward3p;
	size_t off5p;
	while(true) {
		toward3p = (l2r_ == fw);
		assert_geq(al5pf_, al5pi_);
		assert(al5pi_ != 0 || al5pf_ != q.length() - 1);
		if(toward3p) {
			if(al5pf_ == q.length()-1) {
				l2r_ = !l2r_;
				continue;
			}
			if(al5pi_ == al5pf_) {
				off5p = off5p_i = al5pi_;
			} else {
				off5p = off5p_i = (al5pf_ + 1);
			}
		} else {
			if(al5pi_ == 0) {
				l2r_ = !l2r_;
				continue;
			}
			assert_gt(al5pi_, 0);
			if(al5pi_ == al5pf_) {
				off5p = off5p_i = al5pi_;
			} else {
				off5p = off5p_i = (al5pi_ - 1);
			}
		}
		break;
	}
	size_t off3p = q.length() - off5p - 1;
	assert_lt(off5p, q.length());
	assert_lt(off3p, q.length());
	bool firstPos = true;
	assert_eq(0, len_);
    
	// Number of times pf.alloc() is called.  So we can sanity check it.
	size_t nalloc = 0;
    // Set to true as soon as we encounter a branch point along this descent.
    branches = false;
    // hitEnd is set to true iff this edit pushes us to the extreme 5' or 3'
    // end of the alignment
    hitEnd = false;
    // done is set to true iff this edit aligns the only remaining character of
    // the read
    done = false;
	if(root()) {
        assert_eq(al5pi_, al5pf_);
		// Check whether/how far we can jump using ftab
		int ftabLen = ebwtFw.eh().ftabChars();
		bool ftabFits = true;
		if(toward3p && ftabLen + off5p > q.length()) {
			ftabFits = false;
		} else if(!toward3p && off5p < ftabLen) {
			ftabFits = false;
		}
		if(ftabLen > 1 && ftabLen <= nobranchDepth && ftabFits) {
			// Forward index: right-to-left
			size_t off_r2l = fw ? off5p : q.length() - off5p - 1;
			if(l2r_) {
				//
			} else {
				assert_geq(off_r2l, ftabLen - 1);
				off_r2l -= (ftabLen - 1);
			}
			ebwtFw.ftabLoHi(fw ? *q.seq : *q.seqrc, off_r2l,
			                false, // reverse
							topf, botf);
			if(botf - topf == 0) {
				return false;
			}
			int c_r2l = fw ? (*q.seq)[off_r2l] : (*q.seqrc)[off_r2l];
			// Backward index: left-to-right
			size_t off_l2r = fw ? off5p : q.length() - off5p - 1;
			if(l2r_) {
				//
			} else {
				assert_geq(off_l2r, ftabLen - 1);
				off_l2r -= (ftabLen - 1);
			}
			ebwtBw.ftabLoHi(fw ? *q.seq : *q.seqrc, off_l2r,
			                false, // don't reverse
							topb, botb);
			int c_l2r = fw ? (*q.seq)[off_l2r + ftabLen - 1] : (*q.seqrc)[off_l2r + ftabLen - 1];
			assert_eq(botf - topf, botb - topb);
			if(toward3p) {
				assert_geq(off3p, ftabLen - 1);
				off5p += ftabLen; off3p -= ftabLen;
			} else {
				assert_geq(off5p, ftabLen - 1);
				off5p -= ftabLen; off3p += ftabLen;
			}
			len_ += ftabLen;
            if(toward3p) {
				// By convention, al5pf_ and al5pi_ start out equal, so we only
				// advance al5pf_ by ftabLen - 1 (not ftabLen)
                al5pf_ += (ftabLen - 1); // -1 accounts for inclusive al5pf_
                if(al5pf_ == q.length() - 1) {
                    hitEnd = true;
                    done = (al5pi_ == 0);
                }
            } else {
				// By convention, al5pf_ and al5pi_ start out equal, so we only
				// advance al5pi_ by ftabLen - 1 (not ftabLen)
                al5pi_ -= (ftabLen - 1);
                if(al5pi_ == 0) {
                    hitEnd = true;
                    done = (al5pf_ == q.length()-1);
                }
            }
			// Allocate DescentPos data structures and leave them empty.  We
			// jumped over them by doing our lookup in the ftab, so we have no
			// info about outgoing edges from them, besides the matching
			// outgoing edge from the last pos which is in topf/botf and
			// topb/botb.
			size_t id = 0;
			if(firstPos) {
				posid_ = pf.alloc();
				pf[posid_].reset();
				firstPos = false;
				for(int i = 1; i < ftabLen; i++) {
					id = pf.alloc();
					pf[id].reset();
				}
			} else {
				for(int i = 0; i < ftabLen; i++) {
					id = pf.alloc();
					pf[id].reset();
				}
			}
			assert_eq(botf-topf, botb-topb);
			pf[id].c = l2r_ ? c_l2r : c_r2l;
			pf[id].topf[l2r_ ? c_l2r : c_r2l] = topf;
			pf[id].botf[l2r_ ? c_l2r : c_r2l] = botf;
			pf[id].topb[l2r_ ? c_l2r : c_r2l] = topb;
			pf[id].botb[l2r_ ? c_l2r : c_r2l] = botb;
			assert(pf[id].inited());
			nalloc += ftabLen;
		} else {
			// Can't use ftab, use fchr instead
			int rdc = q.getc(off5p, fw);
			assert_range(0, 3, rdc);
			topf = topb = ebwtFw.fchr()[rdc];
			botf = botb = ebwtFw.fchr()[rdc+1];
			if(botf - topf == 0) {
				return false;
			}
			if(toward3p) {
				off5p++; off3p--;
			} else {
				off5p--; off3p++;
			}
			len_++;
            if(toward3p) {
                if(al5pf_ == q.length()-1) {
                    hitEnd = true;
                    done = (al5pi_ == 0);
                }
            } else {
                if(al5pi_ == 0) {
                    hitEnd = true;
                    done = (al5pf_ == q.length()-1);
                }
            }
			// Allocate DescentPos data structure.  We could fill it with the
			// four ranges from fchr if we wanted to, but that will never be
			// relevant.
			size_t id = 0;
			if(firstPos) {
				posid_ = id = pf.alloc();
                firstPos = false;
			} else {
				id = pf.alloc();
			}
			assert_eq(botf-topf, botb-topb);
			pf[id].c = rdc;
			pf[id].topf[rdc] = topf;
			pf[id].botf[rdc] = botf;
			pf[id].topb[rdc] = topb;
			pf[id].botb[rdc] = botb;
			assert(pf[id].inited());
			nalloc++;
		}
		assert_gt(botf, topf);
		assert_eq(botf - topf, botb - topb);
		// Check if this is redundant with an already-explored path
		if(!re.check(fw, al5pi_, al5pf_, al5pf_ - al5pi_ + 1 + gapadd_, topf, botf, pen_)) {
			return false;
		}
	}
    if(done) {
        Edit eempty;
        alsink.reportAlignment(
            q,        // query
			ebwtFw,   // forward index
			ebwtBw,   // backward index
			topf,     // top of SA range in forward index
			botf,     // bottom of SA range in forward index
			topb,     // top of SA range in backward index
			botb,     // bottom of SA range in backward index
            descid_,  // Descent at the leaf
			rid_,     // root id
            eempty,   // extra edit, if necessary
            pen_,     // penalty
            df,       // factory with Descent
            pf,       // factory with DescentPoss
            rs,       // roots
            cs);      // configs
        return true;
    } else if(hitEnd) {
        assert_gt(botf, topf);
        topf_bounce = topf;
        botf_bounce = botf;
        topb_bounce = topb;
        botb_bounce = botb;
        return true; // Bounced
    }
    // We just advanced either ftabLen characters, or 1 character,
    // depending on whether we used ftab or fchr.
    nextLocsBi(ebwtFw, ebwtBw, tloc, bloc, topf, botf, topb, botb);
    assert(tloc.valid());
	assert(botf - topf == 1 ||  bloc.valid());
	assert(botf - topf > 1  || !bloc.valid());
	TIndexOff t[4], b[4];   // dest BW ranges
	TIndexOff tp[4], bp[4]; // dest BW ranges for "prime" index
	ASSERT_ONLY(TIndexOff lasttot = botf - topf);
	bool fail = false;
	while(!fail && !hitEnd) {
        assert(!done);
		int rdc = q.getc(off5p, fw);
		assert_range(0, 3, rdc); // TODO: handle Ns
		assert_gt(botf, topf);
		assert(botf - topf == 1 ||  bloc.valid());
		assert(botf - topf > 1  || !bloc.valid());
		assert(tloc.valid());
        TIndexOff width = botf - topf;
		bool ltr = l2r_;
		const Ebwt& ebwt = ltr ? ebwtBw : ebwtFw;
		t[0] = t[1] = t[2] = t[3] = b[0] = b[1] = b[2] = b[3] = 0;
		if(bloc.valid()) {
			// Set up initial values for the primes
			if(ltr) {
				tp[0] = tp[1] = tp[2] = tp[3] = topf;
				bp[0] = bp[1] = bp[2] = bp[3] = botf;
			} else {
				tp[0] = tp[1] = tp[2] = tp[3] = topb;
				bp[0] = bp[1] = bp[2] = bp[3] = botb;
			}
			// Range delimited by tloc/bloc has size >1.  If size == 1,
			// we use a simpler query (see if(!bloc.valid()) blocks below)
			met.bwops++;
			met.bwops_bi++;
			ebwt.mapBiLFEx(tloc, bloc, t, b, tp, bp);
			// t, b, tp and bp now filled
			ASSERT_ONLY(TIndexOff tot = (b[0]-t[0])+(b[1]-t[1])+(b[2]-t[2])+(b[3]-t[3]));
			ASSERT_ONLY(TIndexOff totp = (bp[0]-tp[0])+(bp[1]-tp[1])+(bp[2]-tp[2])+(bp[3]-tp[3]));
			assert_eq(tot, totp);
			assert_leq(tot, lasttot);
			ASSERT_ONLY(lasttot = tot);
			fail = (b[rdc] <= t[rdc]);
            if(b[rdc] - t[rdc] < width) {
                branches = true;
            }
		} else {
			tp[0] = tp[1] = tp[2] = tp[3] = bp[0] = bp[1] = bp[2] = bp[3] = 0;
			// Range delimited by tloc/bloc has size 1
			TIndexOff ntop = ltr ? topb : topf;
			met.bwops++;
			met.bwops_1++;
			int cc = ebwt.mapLF1(ntop, tloc);
			assert_range(-1, 3, cc);
            fail = (cc != rdc);
            if(fail) {
                branches = true;
            }
			if(cc >= 0) {
				t[cc] = ntop; b[cc] = ntop+1;
				tp[cc] = ltr ? topf : topb;
				bp[cc] = ltr ? botf : botb;
			}
		}
        topf = ltr ? tp[rdc] : t[rdc];
        botf = ltr ? bp[rdc] : b[rdc];
        topb = ltr ? t[rdc] : tp[rdc];
        botb = ltr ? b[rdc] : bp[rdc];
		assert_eq(botf - topf, botb - topb);
        // If 'fail' is true, we failed to align this read character.  We still
        // install the SA ranges into the DescentPos and increment len_ in this
        // case.
        
		// Convert t, tp, b, bp info tf, bf, tb, bb
		TIndexOff *tf = ltr ? tp : t;
		TIndexOff *bf = ltr ? bp : b;
		TIndexOff *tb = ltr ? t : tp;
		TIndexOff *bb = ltr ? b : bp;
		// Allocate DescentPos data structure.
		if(firstPos) {
			posid_ = pf.alloc();
            firstPos = false;
		} else {
			pf.alloc();
		}
		nalloc++;
		pf[posid_ + len_].reset();
		assert_range(0, 3, rdc);
        pf[posid_ + len_].c = rdc;
		for(size_t i = 0; i < 4; i++) {
			pf[posid_ + len_].topf[i] = tf[i];
			pf[posid_ + len_].botf[i] = bf[i];
			pf[posid_ + len_].topb[i] = tb[i];
			pf[posid_ + len_].botb[i] = bb[i];
			assert_eq(pf[posid_ + len_].botf[i] - pf[posid_ + len_].topf[i],
			          pf[posid_ + len_].botb[i] - pf[posid_ + len_].topb[i]);
		}
		if(!fail) {
			// Check if this is redundant with an already-explored path
			size_t al5pf = al5pf_, al5pi = al5pi_;
			if(toward3p) {
				al5pf++;
			} else {
				al5pi--;
			}
			fail = !re.check(fw, al5pi, al5pf, al5pf - al5pi + 1 + gapadd_, topf, botf, pen_);
		}
		if(!fail) {
			len_++;
			if(toward3p) {
				al5pf_++;
				off5p++;
				off3p--;
				if(al5pf_ == q.length() - 1) {
					hitEnd = true;
					done = (al5pi_ == 0);
				}
			} else {
				assert_gt(al5pi_, 0);
				al5pi_--;
				off5p--;
				off3p++;
				if(al5pi_ == 0) {
					hitEnd = true;
					done = (al5pf_ == q.length() - 1);
				}
			}
		}
        if(!fail && !hitEnd) {
            nextLocsBi(ebwtFw, ebwtBw, tloc, bloc, tf[rdc], bf[rdc], tb[rdc], bb[rdc]);
        }
	}
	assert_geq(al5pf_, al5pi_);
	assert(!root() || al5pf_ - al5pi_ + 1 == nalloc || al5pf_ - al5pi_ + 2 == nalloc);
	assert_geq(pf.size(), nalloc);
    if(done) {
        Edit eempty;
        alsink.reportAlignment(
            q,        // query
			ebwtFw,   // forward index
			ebwtBw,   // backward index
			topf,     // top of SA range in forward index
			botf,     // bottom of SA range in forward index
			topb,     // top of SA range in backward index
			botb,     // bottom of SA range in backward index
            descid_,  // Descent at the leaf
			rid_,     // root id
            eempty,   // extra edit, if necessary
            pen_,     // penalty
            df,       // factory with Descent
            pf,       // factory with DescentPoss
            rs,       // roots
            cs);      // configs
        return true;
    } else if(hitEnd) {
        assert_gt(botf, topf);
        topf_bounce = topf;
        botf_bounce = botf;
        topb_bounce = topb;
        botb_bounce = botb;
        return true; // Bounced
    }
    assert(repOk(&q));
	return true;
}

#ifdef ALIGNER_SEED2_MAIN

#include <string>
#include "sstring.h"

using namespace std;

/**
 * A way of feeding simply tests to the seed alignment infrastructure.
 */
int main(int argc, char **argv) {

    EList<string> strs;
    //                            GCTATATAGCGCGCTCGCATCATTTTGTGT
    strs.push_back(string("CATGTCAGCTATATAGCGCGCTCGCATCATTTTGTGTGTAAACCA"
                          "NNNNNNNNNN"
                          "CATGTCAGCTATATAGCGCGCTCGCATCATTTTGTGTGTAAACCA"));
    //                            GCTATATAGCGCGCTTGCATCATTTTGTGT
    //                                           ^
    bool packed = false;
    int color = 0;
	pair<Ebwt*, Ebwt*> ebwts = Ebwt::fromStrings<SString<char> >(
		strs,
		packed,
		color,
		REF_READ_REVERSE,
		Ebwt::default_bigEndian,
		Ebwt::default_lineRate,
		Ebwt::default_offRate,
		Ebwt::default_ftabChars,
		".aligner_seed2.cpp.tmp",
		Ebwt::default_useBlockwise,
		Ebwt::default_bmax,
		Ebwt::default_bmaxMultSqrt,
		Ebwt::default_bmaxDivN,
		Ebwt::default_dcv,
		Ebwt::default_seed,
		false,  // verbose
		false,  // autoMem
		false); // sanity
    
    ebwts.first->loadIntoMemory (color, -1, true, true, true, true, false);
    ebwts.second->loadIntoMemory(color,  1, true, true, true, true, false);
	
	int testnum = 0;
    
	// Query is longer than ftab and matches exactly twice
    for(int i = 0; i < 2; i++) {
		cerr << "Test " << (++testnum) << endl;
		cerr << "  Query with length greater than ftab" << endl;
        DescentMetrics mets;
        DescentDriver dr;
        
        // Set up the read
        BTDnaString seq ("GCTATATAGCGCGCTCGCATCATTTTGTGT", true);
        BTString    qual("ABCDEFGHIabcdefghiABCDEFGHIabc");
        BTDnaString seqrc = seq;
        BTString    qualrc = qual;
        seqrc.reverseComp();
        qualrc.reverse();
        dr.initRead(
            seq,
            qual,
            seqrc,
            qualrc);
        
        // Set up the DescentConfig
        DescentConfig conf;
        conf.cons.init(SIMPLE_FUNC_LINEAR, 0.0, 1.0);
        conf.expol = DESC_EX_NONE;
        
        // Set up the search roots
        dr.addRoot(
            conf,   // DescentConfig
            (i == 0) ? 0 : (seq.length() - 1), // 5' offset into read of root
            (i == 0) ? true : false,           // left-to-right?
            true,   // forward?
            0.0);   // root priority
        
        // Do the search
        Scoring sc = Scoring::base1();
        dr.go(sc, *ebwts.first, *ebwts.second, mets);
		
		// Confirm that an exact-matching alignment was found
		assert_eq(1, dr.sink().nrange());
		assert_eq(2, dr.sink().nelt());
    }
	
	// Query has length euqal to ftab and matches exactly twice
    for(int i = 0; i < 2; i++) {
		cerr << "Test " << (++testnum) << endl;
		cerr << "  Query with length equal to ftab" << endl;
        DescentMetrics mets;
        DescentDriver dr;
        
        // Set up the read
        BTDnaString seq ("GCTATATAGC", true);
        BTString    qual("ABCDEFGHIa");
        BTDnaString seqrc = seq;
        BTString    qualrc = qual;
        seqrc.reverseComp();
        qualrc.reverse();
        dr.initRead(
            seq,
            qual,
            seqrc,
            qualrc);
        
        // Set up the DescentConfig
        DescentConfig conf;
        conf.cons.init(SIMPLE_FUNC_LINEAR, 0.0, 1.0);
        conf.expol = DESC_EX_NONE;
        
        // Set up the search roots
        dr.addRoot(
            conf,   // DescentConfig
            (i == 0) ? 0 : (seq.length() - 1), // 5' offset into read of root
            (i == 0) ? true : false,           // left-to-right?
            true,   // forward?
            0.0);   // root priority
        
        // Do the search
        Scoring sc = Scoring::base1();
        dr.go(sc, *ebwts.first, *ebwts.second, mets);
		
		// Confirm that an exact-matching alignment was found
		assert_eq(1, dr.sink().nrange());
		assert_eq(2, dr.sink().nelt());
    }

	// Query has length less than ftab length and matches exactly twice
    for(int i = 0; i < 2; i++) {
		cerr << "Test " << (++testnum) << endl;
		cerr << "  Query with length less than ftab" << endl;
        DescentMetrics mets;
        DescentDriver dr;
        
        // Set up the read
        BTDnaString seq ("GCTATATAG", true);
        BTString    qual("ABCDEFGHI");
        BTDnaString seqrc = seq;
        BTString    qualrc = qual;
        seqrc.reverseComp();
        qualrc.reverse();
        dr.initRead(
            seq,
            qual,
            seqrc,
            qualrc);
        
        // Set up the DescentConfig
        DescentConfig conf;
        conf.cons.init(SIMPLE_FUNC_LINEAR, 0.0, 1.0);
        conf.expol = DESC_EX_NONE;
        
        // Set up the search roots
        dr.addRoot(
            conf,   // DescentConfig
            (i == 0) ? 0 : (seq.length() - 1), // 5' offset into read of root
            (i == 0) ? true : false,           // left-to-right?
            true,   // forward?
            0.0);   // root priority
        
        // Do the search
        Scoring sc = Scoring::base1();
        dr.go(sc, *ebwts.first, *ebwts.second, mets);
		
		// Confirm that an exact-matching alignment was found
		assert_eq(1, dr.sink().nrange());
		assert_eq(2, dr.sink().nelt());
    }
	
	// Search root is in the middle of the read, requiring a bounce
    for(int i = 0; i < 2; i++) {
		cerr << "Test " << (++testnum) << endl;
		cerr << "  Search root in middle of read" << endl;
        DescentMetrics mets;
        DescentDriver dr;
        
        // Set up the read
		//                012345678901234567890123456789
        BTDnaString seq ("GCTATATAGCGCGCTCGCATCATTTTGTGT", true);
        BTString    qual("ABCDEFGHIabcdefghiABCDEFGHIabc");
		uint32_t top, bot;
		top = bot = 0;
		bool ret = ebwts.first->contains("GCGCTCGCATCATTTTGTGT", &top, &bot);
		cerr << ret << ", " << top << ", " << bot << endl;
        BTDnaString seqrc = seq;
        BTString    qualrc = qual;
        seqrc.reverseComp();
        qualrc.reverse();
        dr.initRead(
            seq,
            qual,
            seqrc,
            qualrc);
        
        // Set up the DescentConfig
        DescentConfig conf;
        conf.cons.init(SIMPLE_FUNC_LINEAR, 0.0, 1.0);
        conf.expol = DESC_EX_NONE;
        
        // Set up the search roots
        dr.addRoot(
            conf,   // DescentConfig
            (i == 0) ? 10 : (seq.length() - 1 - 10), // 5' offset into read of root
            (i == 0) ? true : false,                 // left-to-right?
            true,   // forward?
            0.0);   // root priority
        
        // Do the search
        Scoring sc = Scoring::base1();
        dr.go(sc, *ebwts.first, *ebwts.second, mets);
		
		// Confirm that an exact-matching alignment was found
		assert_eq(1, dr.sink().nrange());
		assert_eq(2, dr.sink().nelt());
    }

	delete ebwts.first;
	delete ebwts.second;
	
	strs.clear();
    strs.push_back(string("CATGTCAGCTATATAGCGCGCTCGCATCATTTTGTGTGTAAACCA"
                          "NNNNNNNNNN"
                          "CATGTCAGCTATATAGCG"));
	ebwts = Ebwt::fromStrings<SString<char> >(
		strs,
		packed,
		color,
		REF_READ_REVERSE,
		Ebwt::default_bigEndian,
		Ebwt::default_lineRate,
		Ebwt::default_offRate,
		Ebwt::default_ftabChars,
		".aligner_seed2.cpp.tmp",
		Ebwt::default_useBlockwise,
		Ebwt::default_bmax,
		Ebwt::default_bmaxMultSqrt,
		Ebwt::default_bmaxDivN,
		Ebwt::default_dcv,
		Ebwt::default_seed,
		false,  // verbose
		false,  // autoMem
		false); // sanity
    
    ebwts.first->loadIntoMemory (color, -1, true, true, true, true, false);
    ebwts.second->loadIntoMemory(color,  1, true, true, true, true, false);
	
	// Query is longer than ftab and matches exactly once.  One search root for
	// forward read.
	{
		size_t last_topf = std::numeric_limits<size_t>::max();
		size_t last_botf = std::numeric_limits<size_t>::max();
		for(int i = 0; i < 2; i++) {
			BTDnaString seq ("GCTATATAGCGCGCTCGCATCATTTTGTGT", true);
			BTString    qual("ABCDEFGHIabcdefghiABCDEFGHIabc");
			for(size_t j = 0; j < seq.length(); j++) {
				cerr << "Test " << (++testnum) << endl;
				cerr << "  Query with length greater than ftab and matches exactly once" << endl;
				DescentMetrics mets;
				DescentDriver dr;
				
				// Set up the read
				BTDnaString seqrc = seq;
				BTString    qualrc = qual;
				seqrc.reverseComp();
				qualrc.reverse();
				dr.initRead(
					seq,
					qual,
					seqrc,
					qualrc);
				
				// Set up the DescentConfig
				DescentConfig conf;
				conf.cons.init(SIMPLE_FUNC_LINEAR, 0.0, 1.0);
				conf.expol = DESC_EX_NONE;
				
				// Set up the search roots
				dr.addRoot(
					conf,   // DescentConfig
					j,      // 5' offset into read of root
					i == 0, // left-to-right?
					true,   // forward?
					0.0);   // root priority
				
				// Do the search
				Scoring sc = Scoring::base1();
				dr.go(sc, *ebwts.first, *ebwts.second, mets);
				
				// Confirm that an exact-matching alignment was found
				assert_eq(1, dr.sink().nrange());
				assert(last_topf == std::numeric_limits<size_t>::max() || last_topf == dr.sink()[0].topf);
				assert(last_botf == std::numeric_limits<size_t>::max() || last_botf == dr.sink()[0].botf);
				assert_eq(1, dr.sink().nelt());
			}
		}
	}

	// Query is longer than ftab and its reverse complement matches exactly
	// once.  Search roots on forward and reverse-comp reads.
	{
		size_t last_topf = std::numeric_limits<size_t>::max();
		size_t last_botf = std::numeric_limits<size_t>::max();
		for(int i = 0; i < 2; i++) {
			BTDnaString seq ("GCTATATAGCGCGCTCGCATCATTTTGTGT", true);
			BTString    qual("ABCDEFGHIabcdefghiABCDEFGHIabc");
			for(size_t j = 0; j < seq.length(); j++) {
				cerr << "Test " << (++testnum) << endl;
				cerr << "  Query with length greater than ftab and reverse complement matches exactly once" << endl;
				DescentMetrics mets;
				DescentDriver dr;
				
				// Set up the read
				BTDnaString seqrc = seq;
				BTString    qualrc = qual;
				seqrc.reverseComp();
				qualrc.reverse();
				dr.initRead(
					seq,
					qual,
					seqrc,
					qualrc);
				
				// Set up the DescentConfig
				DescentConfig conf;
				conf.cons.init(SIMPLE_FUNC_LINEAR, 0.0, 1.0);
				conf.expol = DESC_EX_NONE;
				
				// Set up the search roots
				dr.addRoot(
					conf,   // DescentConfig
					j,      // 5' offset into read of root
					i == 0, // left-to-right?
					true,   // forward?
					0.0);   // root priority
				dr.addRoot(
					conf,   // DescentConfig
					j,      // 5' offset into read of root
					i == 0, // left-to-right?
					false,  // forward?
					1.0);   // root priority
				
				// Do the search
				Scoring sc = Scoring::base1();
				dr.go(sc, *ebwts.first, *ebwts.second, mets);
				
				// Confirm that an exact-matching alignment was found
				assert_eq(1, dr.sink().nrange());
				assert(last_topf == std::numeric_limits<size_t>::max() || last_topf == dr.sink()[0].topf);
				assert(last_botf == std::numeric_limits<size_t>::max() || last_botf == dr.sink()[0].botf);
				assert_eq(1, dr.sink().nelt());
			}
		}
	}

	// Query is longer than ftab and matches exactly once with one mismatch
	{
		size_t last_topf = std::numeric_limits<size_t>::max();
		size_t last_botf = std::numeric_limits<size_t>::max();
		for(int i = 0; i < 2; i++) {
			// Set up the read
			//    Ref: CATGTCAGCTATATAGCGCGCTCGCATCATTTTGTGTGTAAACCA
			//                ||||||||||||||||||||||||||||||
			BTDnaString orig("GCTATATAGCGCGCTCGCATCATTTTGTGT", true);
			//                012345678901234567890123456789
			BTString    qual("ABCDEFGHIabcdefghiABCDEFGHIabc");
			for(size_t k = 0; k < orig.length(); k++) {
				BTDnaString seq = orig;
				seq.set(seq[k] ^ 3, k);
				for(size_t j = 0; j < seq.length(); j++) {
					// Assume left-to-right
					size_t beg = j;
					size_t end = j + Ebwt::default_ftabChars;
					// Mismatch penalty is 3, so we have to skip starting
					// points that are within 2 from the mismatch
					if((i > 0 && j > 0) || j == seq.length()-1) {
						// Right-to-left
						if(beg < Ebwt::default_ftabChars) {
							beg = 0;
						} else {
							beg -= Ebwt::default_ftabChars;
						}
						end -= Ebwt::default_ftabChars;
					}
					if(beg <= k && end > k) {
						continue;
					}
					if((j > k) ? (j - k <= 2) : (k - j <= 2)) {
						continue;
					}
					cerr << "Test " << (++testnum) << endl;
					cerr << "  Query with length greater than ftab and matches exactly once with 1mm" << endl;
					DescentMetrics mets;
					DescentDriver dr;
					
					BTDnaString seqrc = seq;
					BTString    qualrc = qual;
					seqrc.reverseComp();
					qualrc.reverse();
					dr.initRead(
						seq,
						qual,
						seqrc,
						qualrc);
					
					// Set up the DescentConfig
					DescentConfig conf;
					// Changed 
					conf.cons.init(SIMPLE_FUNC_LINEAR, 0.0, 1.0);
					conf.expol = DESC_EX_NONE;
					
					// Set up the search roots
					dr.addRoot(
						conf,   // DescentConfig
						j,      // 5' offset into read of root
						i == 0, // left-to-right?
						true,   // forward?
						0.0);   // root priority
					
					// Do the search
					Scoring sc = Scoring::base1();
					dr.go(sc, *ebwts.first, *ebwts.second, mets);
					
					// Confirm that an exact-matching alignment was found
					assert_eq(1, dr.sink().nrange());
					assert(last_topf == std::numeric_limits<size_t>::max() || last_topf == dr.sink()[0].topf);
					assert(last_botf == std::numeric_limits<size_t>::max() || last_botf == dr.sink()[0].botf);
					cerr << dr.sink()[0].topf << ", " << dr.sink()[0].botf << endl;
					assert_eq(1, dr.sink().nelt());
					last_topf = dr.sink()[0].topf;
					last_botf = dr.sink()[0].botf;
				}
			}
		}
    }

	// Query is longer than ftab and matches exactly once with one mismatch
	{
		RandomSource rnd(77);
		size_t last_topf = std::numeric_limits<size_t>::max();
		size_t last_botf = std::numeric_limits<size_t>::max();
		for(int i = 0; i < 2; i++) {
			// Set up the read
			//    Ref: CATGTCAGCTATATAGCGCGCTCGCATCATTTTGTGTGTAAACCA
			//                ||||||||||||||||||||||||||||||
			BTDnaString orig("GCTATATAGCGCGCTCGCATCATTTTGTGT", true);
			//                012345678901234567890123456789
			BTString    qual("ABCDEFGHIabcdefghiABCDEFGHIabc");
			for(size_t k = 0; k < orig.length(); k++) {
				BTDnaString seq = orig;
				seq.set(seq[k] ^ 3, k);
				cerr << "Test " << (++testnum) << endl;
				cerr << "  Query with length greater than ftab and matches exactly once with 1mm.  Many search roots." << endl;
				DescentMetrics mets;
				DescentDriver dr;
				
				BTDnaString seqrc = seq;
				BTString    qualrc = qual;
				seqrc.reverseComp();
				qualrc.reverse();
				dr.initRead(
					seq,
					qual,
					seqrc,
					qualrc);
				
				// Set up the DescentConfig
				DescentConfig conf;
				// Changed 
				conf.cons.init(SIMPLE_FUNC_LINEAR, 0.0, 1.0);
				conf.expol = DESC_EX_NONE;
				
				// Set up several random search roots
				bool onegood = false;
				for(size_t y = 0; y < 10; y++) {
					size_t j = rnd.nextU32() % seq.length();
					bool ltr = (rnd.nextU2() == 0) ? true : false;
					bool fw = (rnd.nextU2() == 0) ? true : false;
					dr.addRoot(
						conf,     // DescentConfig
						j,        // 5' offset into read of root
						ltr,      // left-to-right?
						fw,       // forward?
						y * 1.0); // root priority
					// Assume left-to-right
					size_t beg = j;
					size_t end = j + Ebwt::default_ftabChars;
					// Mismatch penalty is 3, so we have to skip starting
					// points that are within 2 from the mismatch
					if(!ltr) {
						// Right-to-left
						if(beg < Ebwt::default_ftabChars) {
							beg = 0;
						} else {
							beg -= Ebwt::default_ftabChars;
						}
						end -= Ebwt::default_ftabChars;
					}
					bool good = true;
					if(!fw) {
						good = false;
					}
					if(beg <= k && end > k) {
						good = false;
					}
					if((j > k) ? (j - k <= 2) : (k - j <= 2)) {
						good = false;
					}
					if(good) {
						onegood = true;
					}
				}
				if(!onegood) {
					continue;
				}
				
				// Do the search
				Scoring sc = Scoring::base1();
				dr.go(sc, *ebwts.first, *ebwts.second, mets);
				
				// Confirm that an exact-matching alignment was found
				assert_eq(1, dr.sink().nrange());
				assert(last_topf == std::numeric_limits<size_t>::max() || last_topf == dr.sink()[0].topf);
				assert(last_botf == std::numeric_limits<size_t>::max() || last_botf == dr.sink()[0].botf);
				cerr << dr.sink()[0].topf << ", " << dr.sink()[0].botf << endl;
				assert_eq(1, dr.sink().nelt());
				last_topf = dr.sink()[0].topf;
				last_botf = dr.sink()[0].botf;
			}
		}
    }

	// Query is longer than ftab and matches exactly once with one read gap
	{
		size_t last_topf = std::numeric_limits<size_t>::max();
		size_t last_botf = std::numeric_limits<size_t>::max();
		for(int i = 0; i < 2; i++) {
			// Set up the read
			//    Ref: CATGTCAGCTATATAGCGCGCTCGCATCATTTTGTGTGTAAACCA
			//                |||||||||||||||///////////////
			BTDnaString seq ("GCTATATAGCGCGCTGCATCATTTTGTGT", true);
			//                012345678901234567890123456789
			BTString    qual("ABCDEFGHIabcdefghiABCDEFGHIabc");
			for(size_t j = 0; j < seq.length(); j++) {
				// Assume left-to-right
				size_t beg = j;
				size_t end = j + Ebwt::default_ftabChars;
				// Mismatch penalty is 3, so we have to skip starting
				// points that are within 2 from the mismatch
				if((i > 0 && j > 0) || j == seq.length()-1) {
					// Right-to-left
					if(beg < Ebwt::default_ftabChars) {
						beg = 0;
					} else {
						beg -= Ebwt::default_ftabChars;
					}
					end -= Ebwt::default_ftabChars;
				}
				if(beg <= 15 && end >= 15) {
					continue;
				}
				cerr << "Test " << (++testnum) << endl;
				cerr << "  Query matches once with a read gap of length 1" << endl;
				DescentMetrics mets;
				DescentDriver dr;
				
				BTDnaString seqrc = seq;
				BTString    qualrc = qual;
				seqrc.reverseComp();
				qualrc.reverse();
				dr.initRead(
					seq,
					qual,
					seqrc,
					qualrc);
				
				// Set up the DescentConfig
				DescentConfig conf;
				// Changed 
				conf.cons.init(SIMPLE_FUNC_LINEAR, 0.0, 1.5);
				conf.expol = DESC_EX_NONE;
				
				// Set up the search roots
				dr.addRoot(
					conf,   // DescentConfig
					j,      // 5' offset into read of root
					i == 0, // left-to-right?
					true,   // forward?
					0.0);   // root priority
				
				// Do the search
				Scoring sc = Scoring::base1();
				dr.go(sc, *ebwts.first, *ebwts.second, mets);
				
				// Confirm that an exact-matching alignment was found
				assert_eq(1, dr.sink().nrange());
				assert_eq(sc.readGapOpen() + 0 * sc.readGapExtend(), dr.sink()[0].pen);
				assert(last_topf == std::numeric_limits<size_t>::max() || last_topf == dr.sink()[0].topf);
				assert(last_botf == std::numeric_limits<size_t>::max() || last_botf == dr.sink()[0].botf);
				cerr << dr.sink()[0].topf << ", " << dr.sink()[0].botf << endl;
				assert_eq(1, dr.sink().nelt());
				last_topf = dr.sink()[0].topf;
				last_botf = dr.sink()[0].botf;
			}
		}
    }

	// Query is longer than ftab and matches exactly once with one read gap of
	// length 3
	{
		size_t last_topf = std::numeric_limits<size_t>::max();
		size_t last_botf = std::numeric_limits<size_t>::max();
		for(int i = 0; i < 2; i++) {
			// Set up the read
			//    Ref: CATGTCAGCTATATAGCGCGCTCGCATCATTTTGTGTGTAAACCA
			//                ||||||||||||||   |||||||||||||
			BTDnaString seq ("GCTATATAGCGCGC" "CATCATTTTGTGT", true);
			//                01234567890123   4567890123456
			BTString    qual("ABCDEFGHIabcde" "fghiABCDEFGHI");
			for(size_t j = 0; j < seq.length(); j++) {
				// Assume left-to-right
				size_t beg = j;
				size_t end = j + Ebwt::default_ftabChars;
				// Mismatch penalty is 3, so we have to skip starting
				// points that are within 2 from the mismatch
				if((i > 0 && j > 0) || j == seq.length()-1) {
					// Right-to-left
					if(beg < Ebwt::default_ftabChars) {
						beg = 0;
					} else {
						beg -= Ebwt::default_ftabChars;
					}
					end -= Ebwt::default_ftabChars;
				}
				if(beg <= 14 && end >= 14) {
					continue;
				}
				cerr << "Test " << (++testnum) << endl;
				cerr << "  Query matches once with a read gap of length 3" << endl;
				DescentMetrics mets;
				DescentDriver dr;
				
				BTDnaString seqrc = seq;
				BTString    qualrc = qual;
				seqrc.reverseComp();
				qualrc.reverse();
				dr.initRead(
					seq,
					qual,
					seqrc,
					qualrc);
				
				// Set up the DescentConfig
				DescentConfig conf;
				// Changed
				conf.cons.init(SIMPLE_FUNC_LINEAR, 0.0, 2.5);
				conf.expol = DESC_EX_NONE;
				
				// Set up the search roots
				dr.addRoot(
					conf,   // DescentConfig
					j,      // 5' offset into read of root
					i == 0, // left-to-right?
					true,   // forward?
					0.0);   // root priority
				
				// Do the search
				Scoring sc = Scoring::base1();
				// Need to adjust the mismatch penalty up to avoid alignments
				// with lots of mismatches.
				sc.setMmPen(COST_MODEL_CONSTANT, 6, 6);
				dr.go(sc, *ebwts.first, *ebwts.second, mets);
				
				// Confirm that an exact-matching alignment was found
				assert_eq(1, dr.sink().nrange());
				assert_eq(sc.readGapOpen() + 2 * sc.readGapExtend(), dr.sink()[0].pen);
				assert(last_topf == std::numeric_limits<size_t>::max() || last_topf == dr.sink()[0].topf);
				assert(last_botf == std::numeric_limits<size_t>::max() || last_botf == dr.sink()[0].botf);
				cerr << dr.sink()[0].topf << ", " << dr.sink()[0].botf << endl;
				assert_eq(1, dr.sink().nelt());
				last_topf = dr.sink()[0].topf;
				last_botf = dr.sink()[0].botf;
			}
		}
    }

	// Query is longer than ftab and matches exactly once with one reference gap
	{
		size_t last_topf = std::numeric_limits<size_t>::max();
		size_t last_botf = std::numeric_limits<size_t>::max();
		for(int i = 0; i < 2; i++) {
			// Set up the read
			//    Ref: CATGTCAGCTATATAGCGCGC" "TCGCATCATTTTGTGTGTAAACCA
			//                ||||||||||||||   ||||||||||||||||
			BTDnaString seq ("GCTATATAGCGCGCA""TCGCATCATTTTGTGT", true);
			//                012345678901234  5678901234567890
			BTString    qual("ABCDEFGHIabcdef""ghiABCDEFGHIabcd");
			for(size_t j = 0; j < seq.length(); j++) {
				// Assume left-to-right
				size_t beg = j;
				size_t end = j + Ebwt::default_ftabChars;
				// Mismatch penalty is 3, so we have to skip starting
				// points that are within 2 from the mismatch
				if((i > 0 && j > 0) || j == seq.length()-1) {
					// Right-to-left
					if(beg < Ebwt::default_ftabChars) {
						beg = 0;
					} else {
						beg -= Ebwt::default_ftabChars;
					}
					end -= Ebwt::default_ftabChars;
				}
				if(beg <= 14 && end >= 14) {
					continue;
				}
				cerr << "Test " << (++testnum) << endl;
				cerr << "  Query matches once with a reference gap of length 1" << endl;
				DescentMetrics mets;
				DescentDriver dr;
				
				BTDnaString seqrc = seq;
				BTString    qualrc = qual;
				seqrc.reverseComp();
				qualrc.reverse();
				dr.initRead(
					seq,
					qual,
					seqrc,
					qualrc);
				
				// Set up the DescentConfig
				DescentConfig conf;
				// Changed 
				conf.cons.init(SIMPLE_FUNC_LINEAR, 0.0, 2.5);
				conf.expol = DESC_EX_NONE;
				
				// Set up the search roots
				dr.addRoot(
					conf,   // DescentConfig
					j,      // 5' offset into read of root
					i == 0, // left-to-right?
					true,   // forward?
					0.0);   // root priority
				
				// Do the search
				Scoring sc = Scoring::base1();
				// Need to adjust the mismatch penalty up to avoid alignments
				// with lots of mismatches.
				sc.setMmPen(COST_MODEL_CONSTANT, 6, 6);
				dr.go(sc, *ebwts.first, *ebwts.second, mets);
				
				// Confirm that an exact-matching alignment was found
				assert_eq(1, dr.sink().nrange());
				assert_eq(sc.refGapOpen() + 0 * sc.refGapExtend(), dr.sink()[0].pen);
				assert(last_topf == std::numeric_limits<size_t>::max() || last_topf == dr.sink()[0].topf);
				assert(last_botf == std::numeric_limits<size_t>::max() || last_botf == dr.sink()[0].botf);
				cerr << dr.sink()[0].topf << ", " << dr.sink()[0].botf << endl;
				assert_eq(1, dr.sink().nelt());
				last_topf = dr.sink()[0].topf;
				last_botf = dr.sink()[0].botf;
			}
		}
    }

	// Query is longer than ftab and matches exactly once with one reference gap
	{
		size_t last_topf = std::numeric_limits<size_t>::max();
		size_t last_botf = std::numeric_limits<size_t>::max();
		for(int i = 0; i < 2; i++) {
			// Set up the read
			//    Ref: CATGTCAGCTATATAGCGCGC"   "TCGCATCATTTTGTGTGTAAACCA
			//                ||||||||||||||     ||||||||||||||||
			BTDnaString seq ("GCTATATAGCGCGCATG""TCGCATCATTTTGTGT", true);
			//                01234567890123456  7890123456789012
			BTString    qual("ABCDEFGHIabcdefgh""iABCDEFGHIabcdef");
			for(size_t j = 0; j < seq.length(); j++) {
				// Assume left-to-right
				size_t beg = j;
				size_t end = j + Ebwt::default_ftabChars;
				// Mismatch penalty is 3, so we have to skip starting
				// points that are within 2 from the mismatch
				if((i > 0 && j > 0) || j == seq.length()-1) {
					// Right-to-left
					if(beg < Ebwt::default_ftabChars) {
						beg = 0;
					} else {
						beg -= Ebwt::default_ftabChars;
					}
					end -= Ebwt::default_ftabChars;
				}
				if(beg <= 14 && end >= 14) {
					continue;
				}
				if(beg <= 15 && end >= 15) {
					continue;
				}
				if(beg <= 16 && end >= 16) {
					continue;
				}
				cerr << "Test " << (++testnum) << endl;
				cerr << "  Query matches once with a reference gap of length 1" << endl;
				DescentMetrics mets;
				DescentDriver dr;
				
				BTDnaString seqrc = seq;
				BTString    qualrc = qual;
				seqrc.reverseComp();
				qualrc.reverse();
				dr.initRead(
					seq,
					qual,
					seqrc,
					qualrc);
				
				// Set up the DescentConfig
				DescentConfig conf;
				// Changed 
				conf.cons.init(SIMPLE_FUNC_LINEAR, 0.0, 2.5);
				conf.expol = DESC_EX_NONE;
				
				// Set up the search roots
				dr.addRoot(
					conf,   // DescentConfig
					j,      // 5' offset into read of root
					i == 0, // left-to-right?
					true,   // forward?
					0.0);   // root priority
				
				// Do the search
				Scoring sc = Scoring::base1();
				// Need to adjust the mismatch penalty up to avoid alignments
				// with lots of mismatches.
				sc.setMmPen(COST_MODEL_CONSTANT, 6, 6);
				dr.go(sc, *ebwts.first, *ebwts.second, mets);
				
				// Confirm that an exact-matching alignment was found
				assert_eq(1, dr.sink().nrange());
				assert_eq(sc.refGapOpen() + 2 * sc.refGapExtend(), dr.sink()[0].pen);
				assert(last_topf == std::numeric_limits<size_t>::max() || last_topf == dr.sink()[0].topf);
				assert(last_botf == std::numeric_limits<size_t>::max() || last_botf == dr.sink()[0].botf);
				cerr << dr.sink()[0].topf << ", " << dr.sink()[0].botf << endl;
				assert_eq(1, dr.sink().nelt());
				last_topf = dr.sink()[0].topf;
				last_botf = dr.sink()[0].botf;
			}
		}
    }

	// Still haven't tested:
	// 1. Alignments with ref gaps
	// 2. Alignments with combos of read/ref gaps and mismatches
	// 3. Multiple search roots

    delete ebwts.first;
    delete ebwts.second;
	
	cerr << "DONE" << endl;
}
#endif


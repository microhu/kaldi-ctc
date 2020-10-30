// cctc/cctc-graph.cc

// Copyright      2015   Johns Hopkins University (author: Daniel Povey)
//                2016   LingoChamp Feiteng

// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.


#include "ctc/ctc-graph.h"
#include "lat/lattice-functions.h"  // for PruneLattice
#include "lat/minimize-lattice.h"   // for minimization
#include "lat/push-lattice.h"       // for minimization

namespace kaldi {
namespace ctc {

void ShiftTransitionIdAndAddBlanks(fst::StdVectorFst *fst) {
  typedef fst::MutableArcIterator<fst::StdVectorFst > IterType;

  fst::StdArc blank_loop_arc;
  blank_loop_arc.ilabel = 1;  // blank plus one.
  blank_loop_arc.olabel = 0;  // epsilon
  blank_loop_arc.weight = fst::StdArc::Weight::One();

  int32 num_states = fst->NumStates();
  fst::StdArc epsilon_blank_arc(0, 0, fst::StdArc::Weight::One(), 0);

  for (int32 state = 0; state < num_states; state++) {
    std::vector<fst::StdArc> self_loop_arcs;
    for (IterType aiter(fst, state); !aiter.Done(); aiter.Next()) {
      fst::StdArc arc(aiter.Value());
      if (arc.nextstate == state) {  // self-loop
        KALDI_ASSERT(arc.ilabel != 0);
        if (arc.olabel == 0)
        {
          self_loop_arcs.push_back(arc);
          continue;
        }
        else
        {
          // split the self-loop arc to a separate node
          // the state will be iterated and have its ilabel++ later
          int32 self_loop_new_state = fst->AddState();
          arc.nextstate = self_loop_new_state;
          fst->AddArc(state, arc);

          fst::StdArc new_self_loop_eps_arc(arc.ilabel, 0, fst::StdArc::Weight::One(), self_loop_new_state);
          fst->AddArc(self_loop_new_state, new_self_loop_eps_arc);
          epsilon_blank_arc.nextstate = state;
          fst->AddArc(self_loop_new_state, epsilon_blank_arc);
        }
      }

      if (arc.ilabel != 0)
      {
        arc.ilabel++;
        aiter.SetValue(arc);
      }
    }

    int32 new_state = fst->AddState();
    // move state.arcs to new_state
    for (IterType aiter(fst, state); !aiter.Done(); aiter.Next()) {
      fst::StdArc arc(aiter.Value());
      if (arc.nextstate != state)
        fst->AddArc(new_state, arc);
    }
    fst->DeleteArcs(state);

    // connect state -> new_state
    epsilon_blank_arc.nextstate = new_state;
    fst->AddArc(state, epsilon_blank_arc);
    // add blank loop arc
    blank_loop_arc.nextstate = new_state;
    fst->AddArc(new_state, blank_loop_arc);

    // add self loop arcs to state
    for (int32 k = 0; k < self_loop_arcs.size(); ++k) {
      self_loop_arcs[k]++;
      fst->AddArc(state, self_loop_arcs[k]);
    }
  }
}


void CtcGraphInfo(const TransitionModel &trans_model,
                  const fst::StdVectorFst &fst) {
  typedef fst::ArcIterator<fst::StdVectorFst > IterType;
  int32 num_states = fst.NumStates();
  for (int32 state = 0; state < num_states; state++) {
    KALDI_LOG << "State " << state;
    for (IterType aiter(fst, state); !aiter.Done(); aiter.Next()) {
      fst::StdArc arc(aiter.Value());
      if (arc.ilabel != 0 && arc.ilabel != 1) {
        KALDI_LOG << "ilabel " << arc.ilabel << ", phone "
                  << trans_model.TransitionIdToPhone(arc.ilabel - 1);
      }
    }
  }
}


// This function, not declared in the header, is used inside
// DeterminizeLatticePhonePrunedCtc.  It is a CCTC version of
// DeterminizeLatticeInsertPhones(), which is defined in
// ../lat/determinize-lattice-pruned.cc.
template<class Weight>
typename fst::ArcTpl<Weight>::Label DeterminizeLatticeInsertPhonesCtc(
  const CtcTransitionModel &trans_model,
  fst::MutableFst<fst::ArcTpl<Weight> > *fst) {
  using namespace fst;
  // Define some types.
  typedef ArcTpl<Weight> Arc;
  typedef typename Arc::StateId StateId;
  typedef typename Arc::Label Label;

  // Work out the first phone symbol. This is more related to the phone
  // insertion function, so we put it here and make it the returning value of
  // DeterminizeLatticeInsertPhones().
  Label first_phone_label = fst::HighestNumberedInputSymbol(*fst) + 1;

  // Insert phones here.
  for (StateIterator<MutableFst<Arc> > siter(*fst);
       !siter.Done(); siter.Next()) {
    StateId state = siter.Value();
    if (state == fst->Start())
      continue;
    for (MutableArcIterator<MutableFst<Arc> > aiter(fst, state);
         !aiter.Done(); aiter.Next()) {
      Arc arc = aiter.Value();

      // Note: the words are on the input symbol side and transition-id's are on
      // the output symbol side.
      int32 phone;
      if ((arc.olabel != 0)
          && ((phone = trans_model.GraphLabelToPhone(arc.olabel)) > 0)
          && ((!trans_model.IsSelfLoop(arc.olabel)))) {
        if (arc.ilabel == 0) {
          // If there is no word on the arc, insert the phone directly.
          arc.ilabel = first_phone_label + phone;
        } else {
          // Otherwise, add an additional arc.
          StateId additional_state = fst->AddState();
          StateId next_state = arc.nextstate;
          arc.nextstate = additional_state;
          fst->AddArc(additional_state,
                      Arc(first_phone_label + phone, 0,
                          Weight::One(), next_state));
        }
      }
      aiter.SetValue(arc);
    }
  }
  return first_phone_label;
}



// this function, not declared in the header, is a 'CCTC' version of
// DeterminizeLatticePhonePrunedFirstPass(), as defined in
// ../lat/determinize-lattice-pruned.cc.  It's only called from
// DeterminizeLatticePhonePrunedCtc().
template<class Weight, class IntType>
bool DeterminizeLatticePhonePrunedFirstPassCtc(
  const CtcTransitionModel &trans_model,
  double beam,
  fst::MutableFst<fst::ArcTpl<Weight> > *fst,
  const fst::DeterminizeLatticePrunedOptions &opts) {
  using namespace fst;
  // First, insert the phones.
  typename ArcTpl<Weight>::Label first_phone_label =
    DeterminizeLatticeInsertPhonesCtc(trans_model, fst);
  TopSort(fst);

  // Second, do determinization with phone inserted.
  bool ans = DeterminizeLatticePruned<Weight>(*fst, beam, fst, opts);

  // Finally, remove the inserted phones.
  // We don't need a special 'CCTC' version of this function.
  DeterminizeLatticeDeletePhones(first_phone_label, fst);
  TopSort(fst);

  return ans;
}


// this function, not declared in the header, is a 'CCTC' version of
// DeterminizeLatticePhonePruned(), as defined in ../lat/determinize-lattice-pruned.cc.
// It's only called from DeterminizeLatticePhonePrunedWrapperCtc().
template<class Weight, class IntType>
bool DeterminizeLatticePhonePrunedCtc(
  const CtcTransitionModel &trans_model,
  fst::MutableFst<fst::ArcTpl<Weight> > *ifst,
  double beam,
  fst::MutableFst<fst::ArcTpl<fst::CompactLatticeWeightTpl<Weight, IntType> > >
  *ofst,
  fst::DeterminizeLatticePhonePrunedOptions opts) {
  using namespace fst;
  // Returning status.
  bool ans = true;

  // Make sure at least one of opts.phone_determinize and opts.word_determinize
  // is not false, otherwise calling this function doesn't make any sense.
  if ((opts.phone_determinize || opts.word_determinize) == false) {
    KALDI_WARN << "Both --phone-determinize and --word-determinize are "
               << "set to false, copying lattice without determinization.";
    // We are expecting the words on the input side.
    ConvertLattice<Weight, IntType>(*ifst, ofst, false);
    return ans;
  }

  // Determinization options.
  DeterminizeLatticePrunedOptions det_opts;
  det_opts.delta = opts.delta;
  det_opts.max_mem = opts.max_mem;

  // If --phone-determinize is true, do the determinization on phone + word
  // lattices.
  if (opts.phone_determinize) {
    KALDI_VLOG(1) << "Doing first pass of determinization on phone + word "
                  << "lattices.";
    ans = DeterminizeLatticePhonePrunedFirstPassCtc<Weight, IntType>(
            trans_model, beam, ifst, det_opts) && ans;

    // If --word-determinize is false, we've finished the job and return here.
    if (!opts.word_determinize) {
      // We are expecting the words on the input side.
      ConvertLattice<Weight, IntType>(*ifst, ofst, false);
      return ans;
    }
  }

  // If --word-determinize is true, do the determinization on word lattices.
  if (opts.word_determinize) {
    KALDI_VLOG(1) << "Doing second pass of determinization on word lattices.";
    ans = DeterminizeLatticePruned<Weight, IntType>(
            *ifst, beam, ofst, det_opts) && ans;
  }

  // If --minimize is true, push and minimize after determinization.
  if (opts.minimize) {
    KALDI_VLOG(1) << "Pushing and minimizing on word lattices.";
    ans = fst::PushCompactLatticeStrings<Weight, IntType>(ofst) && ans;
    ans = fst::PushCompactLatticeWeights<Weight, IntType>(ofst) && ans;
    ans = fst::MinimizeCompactLattice<Weight, IntType>(ofst) && ans;
  }

  return ans;
}


bool DeterminizeLatticePhonePrunedWrapperCtc(
  const CtcTransitionModel &trans_model,
  fst::MutableFst<kaldi::LatticeArc> *ifst,
  double beam,
  fst::MutableFst<kaldi::CompactLatticeArc> *ofst,
  fst::DeterminizeLatticePhonePrunedOptions opts) {
  using namespace fst;
  bool ans = true;
  Invert(ifst);
  if (ifst->Properties(fst::kTopSorted, true) == 0) {
    if (!TopSort(ifst)) {
      // Cannot topologically sort the lattice -- determinization will fail.
      KALDI_ERR << "Topological sorting of state-level lattice failed (probably"
                << " your lexicon has empty words or your LM has epsilon cycles"
                << ").";
    }
  }
  ILabelCompare<kaldi::LatticeArc> ilabel_comp;
  ArcSort(ifst, ilabel_comp);
  ans = DeterminizeLatticePhonePrunedCtc<kaldi::LatticeWeight, kaldi::int32>
        (
          trans_model, ifst, beam, ofst, opts);
  Connect(ofst);
  return ans;
}



}  // namespace ctc
}  // namespace kaldi

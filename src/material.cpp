/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2010 Marco Costalba, Joona Kiiski, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <cassert>
#include <cstring>
#include <map>

#include "material.h"

using namespace std;

namespace {

  // Values modified by Joona Kiiski
  const Value MidgameLimit = Value(15581);
  const Value EndgameLimit = Value(3998);

  // Scale factors used when one side has no more pawns
  const int NoPawnsSF[4] = { 6, 12, 32 };

  // Polynomial material balance parameters
  const Value RedundantQueenPenalty = Value(320);
  const Value RedundantRookPenalty  = Value(554);

  const int LinearCoefficients[6] = { 1617, -162, -1172, -190, 105, 26 };

  const int QuadraticCoefficientsSameColor[][8] = {
  { 7, 7, 7, 7, 7, 7 }, { 39, 2, 7, 7, 7, 7 }, { 35, 271, -4, 7, 7, 7 },
  { 7, 25, 4, 7, 7, 7 }, { -27, -2, 46, 100, 56, 7 }, { 58, 29, 83, 148, -3, -25 } };

  const int QuadraticCoefficientsOppositeColor[][8] = {
  { 41, 41, 41, 41, 41, 41 }, { 37, 41, 41, 41, 41, 41 }, { 10, 62, 41, 41, 41, 41 },
  { 57, 64, 39, 41, 41, 41 }, { 50, 40, 23, -22, 41, 41 }, { 106, 101, 3, 151, 171, 41 } };

  typedef EndgameEvaluationFunctionBase EF;
  typedef EndgameScalingFunctionBase SF;
  typedef map<Key, EF*> EFMap;
  typedef map<Key, SF*> SFMap;

  // Endgame evaluation and scaling functions accessed direcly and not through
  // the function maps because correspond to more then one material hash key.
  EvaluationFunction<KmmKm> EvaluateKmmKm[] = { EvaluationFunction<KmmKm>(WHITE), EvaluationFunction<KmmKm>(BLACK) };
  EvaluationFunction<KXK>   EvaluateKXK[]   = { EvaluationFunction<KXK>(WHITE),   EvaluationFunction<KXK>(BLACK) };
  ScalingFunction<KBPsK>    ScaleKBPsK[]    = { ScalingFunction<KBPsK>(WHITE),    ScalingFunction<KBPsK>(BLACK) };
  ScalingFunction<KQKRPs>   ScaleKQKRPs[]   = { ScalingFunction<KQKRPs>(WHITE),   ScalingFunction<KQKRPs>(BLACK) };
  ScalingFunction<KPsK>     ScaleKPsK[]     = { ScalingFunction<KPsK>(WHITE),     ScalingFunction<KPsK>(BLACK) };
  ScalingFunction<KPKP>     ScaleKPKP[]     = { ScalingFunction<KPKP>(WHITE),     ScalingFunction<KPKP>(BLACK) };

  // Helper templates used to detect a given material distribution
  template<Color Us> bool is_KXK(const Position& pos) {
    const Color Them = (Us == WHITE ? BLACK : WHITE);
    return   pos.non_pawn_material(Them) == VALUE_ZERO
          && pos.piece_count(Them, PAWN) == 0
          && pos.non_pawn_material(Us)   >= RookValueMidgame;
  }

  template<Color Us> bool is_KBPsKs(const Position& pos) {
    return   pos.non_pawn_material(Us)   == BishopValueMidgame
          && pos.piece_count(Us, BISHOP) == 1
          && pos.piece_count(Us, PAWN)   >= 1;
  }

  template<Color Us> bool is_KQKRPs(const Position& pos) {
    const Color Them = (Us == WHITE ? BLACK : WHITE);
    return   pos.piece_count(Us, PAWN)    == 0
          && pos.non_pawn_material(Us)    == QueenValueMidgame
          && pos.piece_count(Us, QUEEN)   == 1
          && pos.piece_count(Them, ROOK)  == 1
          && pos.piece_count(Them, PAWN)  >= 1;
  }
}


/// EndgameFunctions class stores endgame evaluation and scaling functions
/// in two std::map. Because STL library is not guaranteed to be thread
/// safe even for read access, the maps, although with identical content,
/// are replicated for each thread. This is faster then using locks.

class EndgameFunctions {
public:
  EndgameFunctions();
  ~EndgameFunctions();
  template<class T> T* get(Key key) const;

private:
  template<class T> void add(const string& keyCode);

  static Key buildKey(const string& keyCode);
  static const string swapColors(const string& keyCode);

  // Here we store two maps, for evaluate and scaling functions...
  pair<EFMap, SFMap> maps;

  // ...and here is the accessing template function
  template<typename T> const map<Key, T*>& get() const;
};

// Explicit specializations of a member function shall be declared in
// the namespace of which the class template is a member.
template<> const EFMap& EndgameFunctions::get<EF>() const { return maps.first; }
template<> const SFMap& EndgameFunctions::get<SF>() const { return maps.second; }


/// MaterialInfoTable c'tor and d'tor allocate and free the space for EndgameFunctions

MaterialInfoTable::MaterialInfoTable() { funcs = new EndgameFunctions(); }
MaterialInfoTable::~MaterialInfoTable() { delete funcs; }


/// MaterialInfoTable::get_material_info() takes a position object as input,
/// computes or looks up a MaterialInfo object, and returns a pointer to it.
/// If the material configuration is not already present in the table, it
/// is stored there, so we don't have to recompute everything when the
/// same material configuration occurs again.

MaterialInfo* MaterialInfoTable::get_material_info(const Position& pos) const {

  Key key = pos.get_material_key();
  MaterialInfo* mi = find(key);

  // If mi->key matches the position's material hash key, it means that we
  // have analysed this material configuration before, and we can simply
  // return the information we found the last time instead of recomputing it.
  if (mi->key == key)
      return mi;

  // Initialize MaterialInfo entry
  memset(mi, 0, sizeof(MaterialInfo));
  mi->key = key;
  mi->factor[WHITE] = mi->factor[BLACK] = (uint8_t)SCALE_FACTOR_NORMAL;

  // Store game phase
  mi->gamePhase = MaterialInfoTable::game_phase(pos);

  // Let's look if we have a specialized evaluation function for this
  // particular material configuration. First we look for a fixed
  // configuration one, then a generic one if previous search failed.
  if ((mi->evaluationFunction = funcs->get<EF>(key)) != NULL)
      return mi;

  if (is_KXK<WHITE>(pos))
  {
      mi->evaluationFunction = &EvaluateKXK[WHITE];
      return mi;
  }

  if (is_KXK<BLACK>(pos))
  {
      mi->evaluationFunction = &EvaluateKXK[BLACK];
      return mi;
  }

  if (!pos.pieces(PAWN) && !pos.pieces(ROOK) && !pos.pieces(QUEEN))
  {
      // Minor piece endgame with at least one minor piece per side and
      // no pawns. Note that the case KmmK is already handled by KXK.
      assert((pos.pieces(KNIGHT, WHITE) | pos.pieces(BISHOP, WHITE)));
      assert((pos.pieces(KNIGHT, BLACK) | pos.pieces(BISHOP, BLACK)));

      if (   pos.piece_count(WHITE, BISHOP) + pos.piece_count(WHITE, KNIGHT) <= 2
          && pos.piece_count(BLACK, BISHOP) + pos.piece_count(BLACK, KNIGHT) <= 2)
      {
          mi->evaluationFunction = &EvaluateKmmKm[WHITE];
          return mi;
      }
  }

  // OK, we didn't find any special evaluation function for the current
  // material configuration. Is there a suitable scaling function?
  //
  // We face problems when there are several conflicting applicable
  // scaling functions and we need to decide which one to use.
  SF* sf;

  if ((sf = funcs->get<SF>(key)) != NULL)
  {
      mi->scalingFunction[sf->color()] = sf;
      return mi;
  }

  // Generic scaling functions that refer to more then one material
  // distribution. Should be probed after the specialized ones.
  // Note that these ones don't return after setting the function.
  if (is_KBPsKs<WHITE>(pos))
      mi->scalingFunction[WHITE] = &ScaleKBPsK[WHITE];

  if (is_KBPsKs<BLACK>(pos))
      mi->scalingFunction[BLACK] = &ScaleKBPsK[BLACK];

  if (is_KQKRPs<WHITE>(pos))
      mi->scalingFunction[WHITE] = &ScaleKQKRPs[WHITE];

  else if (is_KQKRPs<BLACK>(pos))
      mi->scalingFunction[BLACK] = &ScaleKQKRPs[BLACK];

  Value npm_w = pos.non_pawn_material(WHITE);
  Value npm_b = pos.non_pawn_material(BLACK);

  if (npm_w + npm_b == VALUE_ZERO)
  {
      if (pos.piece_count(BLACK, PAWN) == 0)
      {
          assert(pos.piece_count(WHITE, PAWN) >= 2);
          mi->scalingFunction[WHITE] = &ScaleKPsK[WHITE];
      }
      else if (pos.piece_count(WHITE, PAWN) == 0)
      {
          assert(pos.piece_count(BLACK, PAWN) >= 2);
          mi->scalingFunction[BLACK] = &ScaleKPsK[BLACK];
      }
      else if (pos.piece_count(WHITE, PAWN) == 1 && pos.piece_count(BLACK, PAWN) == 1)
      {
          // This is a special case because we set scaling functions
          // for both colors instead of only one.
          mi->scalingFunction[WHITE] = &ScaleKPKP[WHITE];
          mi->scalingFunction[BLACK] = &ScaleKPKP[BLACK];
      }
  }

  // No pawns makes it difficult to win, even with a material advantage
  if (pos.piece_count(WHITE, PAWN) == 0 && npm_w - npm_b <= BishopValueMidgame)
  {
      mi->factor[WHITE] =
      (npm_w == npm_b || npm_w < RookValueMidgame ? 0 : NoPawnsSF[Min(pos.piece_count(WHITE, BISHOP), 2)]);
  }

  if (pos.piece_count(BLACK, PAWN) == 0 && npm_b - npm_w <= BishopValueMidgame)
  {
      mi->factor[BLACK] =
      (npm_w == npm_b || npm_b < RookValueMidgame ? 0 : NoPawnsSF[Min(pos.piece_count(BLACK, BISHOP), 2)]);
  }

  // Compute the space weight
  if (npm_w + npm_b >= 2 * QueenValueMidgame + 4 * RookValueMidgame + 2 * KnightValueMidgame)
  {
      int minorPieceCount =  pos.piece_count(WHITE, KNIGHT) + pos.piece_count(WHITE, BISHOP)
                           + pos.piece_count(BLACK, KNIGHT) + pos.piece_count(BLACK, BISHOP);

      mi->spaceWeight = minorPieceCount * minorPieceCount;
  }

  // Evaluate the material imbalance. We use PIECE_TYPE_NONE as a place holder
  // for the bishop pair "extended piece", this allow us to be more flexible
  // in defining bishop pair bonuses.
  const int pieceCount[2][8] = {
  { pos.piece_count(WHITE, BISHOP) > 1, pos.piece_count(WHITE, PAWN), pos.piece_count(WHITE, KNIGHT),
    pos.piece_count(WHITE, BISHOP)    , pos.piece_count(WHITE, ROOK), pos.piece_count(WHITE, QUEEN) },
  { pos.piece_count(BLACK, BISHOP) > 1, pos.piece_count(BLACK, PAWN), pos.piece_count(BLACK, KNIGHT),
    pos.piece_count(BLACK, BISHOP)    , pos.piece_count(BLACK, ROOK), pos.piece_count(BLACK, QUEEN) } };

  mi->value = (int16_t)(imbalance<WHITE>(pieceCount) - imbalance<BLACK>(pieceCount)) / 16;
  return mi;
}


/// MaterialInfoTable::imbalance() calculates imbalance comparing piece count of each
/// piece type for both colors.

template<Color Us>
int MaterialInfoTable::imbalance(const int pieceCount[][8]) {

  const Color Them = (Us == WHITE ? BLACK : WHITE);

  int pt1, pt2, pc, vv;
  int value = 0;

  // Redundancy of major pieces, formula based on Kaufman's paper
  // "The Evaluation of Material Imbalances in Chess"
  if (pieceCount[Us][ROOK] > 0)
      value -=  RedundantRookPenalty * (pieceCount[Us][ROOK] - 1)
              + RedundantQueenPenalty * pieceCount[Us][QUEEN];

  // Second-degree polynomial material imbalance by Tord Romstad
  for (pt1 = PIECE_TYPE_NONE; pt1 <= QUEEN; pt1++)
  {
      pc = pieceCount[Us][pt1];
      if (!pc)
          continue;

      vv = LinearCoefficients[pt1];

      for (pt2 = PIECE_TYPE_NONE; pt2 <= pt1; pt2++)
          vv +=  QuadraticCoefficientsSameColor[pt1][pt2] * pieceCount[Us][pt2]
               + QuadraticCoefficientsOppositeColor[pt1][pt2] * pieceCount[Them][pt2];

      value += pc * vv;
  }
  return value;
}


/// MaterialInfoTable::game_phase() calculates the phase given the current
/// position. Because the phase is strictly a function of the material, it
/// is stored in MaterialInfo.

Phase MaterialInfoTable::game_phase(const Position& pos) {

  Value npm = pos.non_pawn_material(WHITE) + pos.non_pawn_material(BLACK);

  if (npm >= MidgameLimit)
      return PHASE_MIDGAME;

  if (npm <= EndgameLimit)
      return PHASE_ENDGAME;

  return Phase(((npm - EndgameLimit) * 128) / (MidgameLimit - EndgameLimit));
}


/// EndgameFunctions member definitions

EndgameFunctions::EndgameFunctions() {

  add<EvaluationFunction<KNNK>  >("KNNK");
  add<EvaluationFunction<KPK>   >("KPK");
  add<EvaluationFunction<KBNK>  >("KBNK");
  add<EvaluationFunction<KRKP>  >("KRKP");
  add<EvaluationFunction<KRKB>  >("KRKB");
  add<EvaluationFunction<KRKN>  >("KRKN");
  add<EvaluationFunction<KQKR>  >("KQKR");
  add<EvaluationFunction<KBBKN> >("KBBKN");

  add<ScalingFunction<KNPK>    >("KNPK");
  add<ScalingFunction<KRPKR>   >("KRPKR");
  add<ScalingFunction<KBPKB>   >("KBPKB");
  add<ScalingFunction<KBPPKB>  >("KBPPKB");
  add<ScalingFunction<KBPKN>   >("KBPKN");
  add<ScalingFunction<KRPPKRP> >("KRPPKRP");
}

EndgameFunctions::~EndgameFunctions() {

    for (EFMap::const_iterator it = maps.first.begin(); it != maps.first.end(); ++it)
        delete it->second;

    for (SFMap::const_iterator it = maps.second.begin(); it != maps.second.end(); ++it)
        delete it->second;
}

Key EndgameFunctions::buildKey(const string& keyCode) {

    assert(keyCode.length() > 0 && keyCode.length() < 8);
    assert(keyCode[0] == 'K');

    string fen;
    bool upcase = false;

    // Build up a fen string with the given pieces, note that
    // the fen string could be of an illegal position.
    for (size_t i = 0; i < keyCode.length(); i++)
    {
        if (keyCode[i] == 'K')
            upcase = !upcase;

        fen += char(upcase ? toupper(keyCode[i]) : tolower(keyCode[i]));
    }
    fen += char(8 - keyCode.length() + '0');
    fen += "/8/8/8/8/8/8/8 w - -";
    return Position(fen, false, 0).get_material_key();
}

const string EndgameFunctions::swapColors(const string& keyCode) {

    // Build corresponding key for the opposite color: "KBPKN" -> "KNKBP"
    size_t idx = keyCode.find('K', 1);
    return keyCode.substr(idx) + keyCode.substr(0, idx);
}

template<class T>
void EndgameFunctions::add(const string& keyCode) {

  typedef typename T::Base F;
  typedef map<Key, F*> M;

  const_cast<M&>(get<F>()).insert(pair<Key, F*>(buildKey(keyCode), new T(WHITE)));
  const_cast<M&>(get<F>()).insert(pair<Key, F*>(buildKey(swapColors(keyCode)), new T(BLACK)));
}

template<class T>
T* EndgameFunctions::get(Key key) const {

  typename map<Key, T*>::const_iterator it = get<T>().find(key);
  return it != get<T>().end() ? it->second : NULL;
}

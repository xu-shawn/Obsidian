#include "search.h"
#include "evaluate.h"
#include "movegen.h"
#include "timeman.h"
#include "threads.h"
#include "tt.h"
#include "uci.h"

#include <cmath>
#include <sstream>

#ifdef USE_AVX2
#include <immintrin.h>
#endif

using namespace Threads;

namespace Search {

  struct SearchLoopInfo {
    Value score;
    Move bestMove;
    int selDepth;
  };

  struct SearchInfo {
    Value staticEval;
    Move playedMove;

    Move killers[2];

    Move pv[MAX_PLY];
    int pvLength;
  };

  Color rootColor;

  Move lastBestMove;
  clock_t lastSearchTimeSpan;
  bool printingEnabled = true;

  uint64_t nodesSearched;

  int selDepth;

  int rootDepth;

  int ply = 0;

  NNUE::Accumulator accumulatorStack[MAX_PLY];

  Position posStack[MAX_PLY];

  Position position;
  MoveList rootMoves;

  int lmrTable[MAX_PLY][MAX_MOVES];

  int mainHistory[COLOR_NB][SQUARE_NB * SQUARE_NB];

  inline int fromTo(Move m) {
    return getMoveSrc(m) * SQUARE_NB + getMoveDest(m);
  }

  void clear() {

    TT::clear();

    memset(mainHistory, 0, sizeof(mainHistory));
  }

  // Called one at engine initialization
  void searchInit() {

    // avoid log(0) because it's negative infinity
    lmrTable[0][0] = 0;

    for (int i = 1; i < MAX_PLY; i++) {
      for (int m = 1; m < MAX_MOVES; m++) {
        lmrTable[i][m] = 0.75 + log(i) * log(m) / 2.25;
      }
    }

    clear();
  }

  NNUE::Accumulator* currentAccumulator() {
    return &accumulatorStack[ply];
  }

  inline void pushPosition() {
    memcpy(&posStack[ply], &position, sizeof(Position));

    memcpy(&accumulatorStack[ply + 1], &accumulatorStack[ply], sizeof(NNUE::Accumulator));

    ply++;
  }

  inline void popPosition() {
    ply--;

    memcpy(&position, &posStack[ply], sizeof(Position));
  }

  template<bool root>
  int perft(int depth) {
    MoveList moves;
    getPseudoLegalMoves(position, &moves);

    if (depth == 1) {
      int n = 0;
      for (int i = 0; i < moves.size(); i++) {
        if (!position.isLegal(moves[i]))
          continue;

        n++;
      }
      return n;
    }

    int n = 0;
    for (int i = 0; i < moves.size(); i++) {
      if (!position.isLegal(moves[i]))
        continue;

      pushPosition();
      position.doMove(moves[i], &accumulatorStack[0]);

      int thisNodes = perft<false>(depth - 1);
      if constexpr (root)
        cout << UCI::move(moves[i]) << " -> " << thisNodes << endl;

      popPosition();

      n += thisNodes;
    }
    return n;
  }

  template int perft<false>(int);
  template int perft<true>(int);

  enum NodeType {
    Root, PV, NonPV
  };

  inline clock_t elapsedTime() {
    return clock() - searchLimits.startTime;
  }

  void checkTime() {
    if (!searchLimits.hasTimeLimit())
      return;

    // never use more than 70~80 % of our time
    double d = 0.7;
    if (searchLimits.inc[rootColor])
      d += 0.1;

    if (elapsedTime() >= d * searchLimits.time[rootColor] - 10) {
      searchState = STOP_PENDING;
    }
  }

  inline void playNullMove(SearchInfo* ss) {
    nodesSearched++;
    if ((nodesSearched % 32768) == 0)
      checkTime();

    ss->playedMove = MOVE_NONE;
    pushPosition();
    position.doNullMove();
  }

  inline void playMove(Move move, SearchInfo* ss) {
    nodesSearched++;
    if ((nodesSearched % 32768) == 0)
      checkTime();

    ss->playedMove = move;
    pushPosition();
    position.doMove(move, &accumulatorStack[ply]);
  }

  inline void cancelMove() {
    popPosition();
  }

  int stat_bonus(int d) {
    return std::min(2 * d * d + 16 * d, 1000);
  }

  void scoreMoves(MoveList& moves, Move ttMove, SearchInfo* ss) {
    Move killer0 = ss->killers[0], killer1 = ss->killers[1];

    for (int i = 0; i < moves.size(); i++) {
      int& moveScore = moves.scores[i];

      Move m = moves[i];
      if (m == ttMove) {
        moveScore = INT_MAX;
        continue;
      }

      // init it to 0
      moveScore = 0;

      if (position.isQuiet(m)) {
        moveScore += mainHistory[position.sideToMove][fromTo(m)] / 200;

        if (m == killer0)
          moveScore += 40;

        if (m == killer1)
          moveScore += 20;
      }

      switch (getMoveType(m)) {
      case MT_NORMAL: {
        const Square from = getMoveSrc(m);
        const Square to = getMoveDest(m);
        const Piece movedPc = position.board[from];
        const Piece capturedPc = position.board[to];

        // mvv
        if (capturedPc != NO_PIECE)
          moveScore += PieceValue[capturedPc];

        break;
      }
      case MT_CASTLING: {
        moveScore += 50;
        break;
      }
      case MT_EN_PASSANT: {
        moveScore += 70;
        break;
      }
      case MT_PROMOTION: {
        const Square to = getMoveDest(m);
        const Piece capturedPc = position.board[to];

        moveScore += PieceValue[getPromoType(m)];

        if (capturedPc != NO_PIECE)
          moveScore += PieceValue[capturedPc];

        break;
      }
      }
    }
  }

  Move nextBestMove(MoveList& moveList, int scannedMoves) {
    int bestMoveI = scannedMoves;

    int bestMoveValue = moveList.scores[bestMoveI];

    int size = moveList.size();
    for (int i = scannedMoves + 1; i < size; i++) {
      int thisValue = moveList.scores[i];
      if (thisValue > bestMoveValue) {
        bestMoveValue = thisValue;
        bestMoveI = i;
      }
    }

    Move result = moveList[bestMoveI];
    moveList.moves[bestMoveI] = moveList.moves[scannedMoves];
    moveList.scores[bestMoveI] = moveList.scores[scannedMoves];
    return result;
  }

  inline TT::Flag flagForTT(bool failsHigh) {
    return failsHigh ? TT::FLAG_LOWER : TT::FLAG_UPPER;
  }

  // Should not be called from Root node
  bool is2FoldRepetition() {
    if (position.halfMoveClock < 4)
      return false;


    for (int i = ply - 2; i >= 0; i -= 2) {
      if (position.key == posStack[i].key)
        return true;
    }

    // Start at last-1 because posStack[0] = seenPositions[last]
    for (int i = seenPositions.size() - 2; i >= 0; --i) {
      if (position.key == seenPositions[i])
        return true;
    }

    return false;
  }

  inline Value makeDrawValue() {
    return Value(int(nodesSearched % 3ULL) - 1);
  }

  template<NodeType nodeType>
  Value qsearch(Value alpha, Value beta, SearchInfo* ss) {
    constexpr bool PvNode = nodeType != NonPV;

    const Color us = position.sideToMove, them = ~us;

    if (position.halfMoveClock >= 100)
      return makeDrawValue();

    bool ttHit;
    TT::Entry* ttEntry = TT::probe(position.key, ttHit);
    TT::Flag ttFlag = ttHit ? ttEntry->getFlag() : TT::NO_FLAG;
    Value ttValue = ttHit ? ttEntry->getValue() : VALUE_NONE;
    Move ttMove = ttHit ? ttEntry->getMove() : MOVE_NONE;

    if (!PvNode) {
      if (ttFlag & flagForTT(ttValue >= beta))
        return ttValue;
    }

    Move bestMove = MOVE_NONE;
    Value bestValue, eval;
    const Value oldAlpha = alpha;

    if (position.checkers) {
      bestValue = -VALUE_INFINITE;
      eval = VALUE_NONE;
    }
    else {

      if (ttHit)
        bestValue = eval = ttEntry->getStaticEval();
      else
        bestValue = eval = Eval::evaluate();

      if (ttFlag & flagForTT(ttValue > bestValue)) {
        bestValue = ttValue;
      }

      if (bestValue >= beta)
        return bestValue;
      if (bestValue > alpha)
        alpha = bestValue;
    }
    bool generateAllMoves = position.checkers;
    MoveList moves;
    if (generateAllMoves)
      getPseudoLegalMoves(position, &moves);
    else
      getAggressiveMoves(position, &moves);

    scoreMoves(moves, ttMove, ss);

    bool foundLegalMoves = false;

    for (int i = 0; i < moves.size(); i++) {
      Move move = nextBestMove(moves, i);
      if (!position.isLegal(move))
        continue;

      foundLegalMoves = true;

      if (!generateAllMoves) {
        if (!position.see_ge(move, Value(-95)))
          continue;
      }

      playMove(move, ss);

      Value value = -qsearch<nodeType>(-beta, -alpha, ss + 1);

      cancelMove();

      if (value > bestValue) {
        bestValue = value;

        if (bestValue > alpha) {
          bestMove = move;

          // value >= beta is always true if beta==alpha+1 and value>alpha
          if (!PvNode || bestValue >= beta) {
            ttEntry->store(position.key, TT::FLAG_LOWER, 0, bestMove, bestValue, eval);
            return bestValue;
          }

          // This is never reached on a NonPV node
          alpha = bestValue;
        }
      }
    }

    if (position.checkers && !foundLegalMoves)
      return Value(ply - VALUE_MATE);

    ttEntry->store(position.key, alpha > oldAlpha ? TT::FLAG_EXACT : TT::FLAG_UPPER, 0, bestMove, bestValue, eval);

    return bestValue;
  }

  inline void updatePV(SearchInfo* ss, Move move) {
    // set the move in the pv
    ss->pv[ply] = move;

    // copy all the moves that follow, from the child pv
    for (int i = ply + 1; i < (ss + 1)->pvLength; i++) {
      ss->pv[i] = (ss + 1)->pv[i];
    }

    ss->pvLength = (ss + 1)->pvLength;
  }

  template<NodeType nodeType>
  Value negaMax(Value alpha, Value beta, int depth, bool cutNode, SearchInfo* ss) {
    constexpr bool PvNode = nodeType != NonPV;
    constexpr bool rootNode = nodeType == Root;

    const Color us = position.sideToMove, them = ~us;

    if (PvNode) {
      // init node
      ss->pvLength = ply;

      if (ply > selDepth)
        selDepth = ply;
    }

    if (searchState == STOP_PENDING)
      return makeDrawValue();

    (ss + 1)->killers[0] = MOVE_NONE;
    (ss + 1)->killers[1] = MOVE_NONE;

    if (!rootNode) {
      if (is2FoldRepetition() || position.halfMoveClock >= 100)
        return makeDrawValue();

      // mate distance pruning
      alpha = (Value)myMax(alpha, ply - VALUE_MATE);
      beta = (Value)myMin(beta, VALUE_MATE - ply - 1);
      if (alpha >= beta)
        return alpha;
    }

    bool ttHit;
    TT::Entry* ttEntry = TT::probe(position.key, ttHit);
    TT::Flag ttFlag = ttHit ? ttEntry->getFlag() : TT::NO_FLAG;
    Value ttValue = ttHit ? ttEntry->getValue() : VALUE_NONE;
    Move ttMove = ttHit ? ttEntry->getMove() : MOVE_NONE;

    if (rootNode) {
      if (!ttMove)
        ttMove = rootMoves[0];
    }

    Value eval;
    Move bestMove = MOVE_NONE;
    Value bestValue = -VALUE_INFINITE;

    if (position.checkers)
      depth = myMax(1, depth + 1);

    if (!PvNode
      && ttEntry->getDepth() >= depth) {

      if (ttFlag & flagForTT(ttValue >= beta))
        return ttValue;
    }

    if (depth <= 0)
      return qsearch<PvNode ? PV : NonPV>(alpha, beta, ss);

    bool improving = false;

    // Static evaluation of the position
    if (position.checkers) {
      ss->staticEval = eval = VALUE_NONE;

      // skip pruning when in check
      goto moves_loop;
    }
    else {
      if (ttHit)
        ss->staticEval = eval = ttEntry->getStaticEval();
      else
        ss->staticEval = eval = Eval::evaluate();

      if (ttFlag & flagForTT(ttValue > eval))
        ss->staticEval = eval = ttValue;

      if ((ss - 2)->staticEval != VALUE_NONE)
        improving = ss->staticEval > (ss - 2)->staticEval;
      else if ((ss - 4)->staticEval != VALUE_NONE)
        improving = ss->staticEval > (ss - 4)->staticEval;
    }

    // depth should always be >= 1 at this point

    // Razoring
    if (eval < alpha - 400 - 500 * depth) {
      Value value = qsearch<NonPV>(alpha-1, alpha, ss);
      if (value < alpha)
        return value;
    }

    // Reverse futility pruning
    if (!PvNode
      && depth < 9
      && abs(eval) < VALUE_TB_WIN_IN_MAX_PLY
      && eval >= beta
      && eval + 120 * improving - 140 * depth >= beta)
      return eval;

    // Null move pruning
    if (!PvNode
      && (ss - 1)->playedMove != MOVE_NONE
      && eval >= beta
      && position.hasNonPawns(position.sideToMove)
      && beta > VALUE_TB_LOSS_IN_MAX_PLY) {

      int R = myMin(int(eval - beta) / 200, 3) + depth / 3 + 4;

      playNullMove(ss);
      Value nullValue = -negaMax<NonPV>(-beta, -beta + 1, depth - R, !cutNode, ss + 1);
      cancelMove();

      if (nullValue >= beta && abs(nullValue) < VALUE_TB_WIN_IN_MAX_PLY) {
        return nullValue;
      }
    }

    // IIR
    if (cutNode && depth >= 4 && !ttMove)
      depth -= 2;

  moves_loop:

    const bool wasInCheck = position.checkers;

    MoveList moves;
    if (rootNode) {
      moves = rootMoves;

      for (int i = 0; i < rootMoves.size(); i++)
        rootMoves.scores[i] = -VALUE_INFINITE;
    }
    else {
      getPseudoLegalMoves(position, &moves);
      scoreMoves(moves, ttMove, ss);
    }

    bool foundLegalMove = false;
    int playedMoves = 0;

    for (int i = 0; i < moves.size(); i++) {
      Move move = nextBestMove(moves, i);

      if (!position.isLegal(move))
        continue;

      foundLegalMove = true;

      if (!rootNode
        && bestValue > VALUE_TB_LOSS_IN_MAX_PLY) {
        bool capture = false;
        if (getMoveType(move) == MT_NORMAL) {
          capture = position.board[getMoveDest(move)] != NO_PIECE;
        }

        if (capture) {
          if (!position.see_ge(move, Value(-260 * depth)))
            continue;
        }
      }

      playMove(move, ss);

      Value value;

      bool needFullSearch;
      if (!wasInCheck && depth >= 3 && playedMoves > (1 + 2 * PvNode)) {
        int R = lmrTable[depth][playedMoves + 1];

        R += !improving;
        R -= PvNode;

        // Do the clamp to avoid a qsearch or an extension in the child search
        int reducedDepth = myClamp(depth - R, 1, depth + 1);

        value = -negaMax<NonPV>(-alpha - 1, -alpha, reducedDepth, true, ss + 1);

        needFullSearch = value > alpha && reducedDepth < depth;
      }
      else
        needFullSearch = !PvNode || playedMoves >= 1;


      if (needFullSearch)
        value = -negaMax<NonPV>(-alpha - 1, -alpha, depth - 1, !cutNode, ss + 1);

      if (PvNode && (playedMoves == 0 || value > alpha))
        value = -negaMax<PV>(-beta, -alpha, depth - 1, false, ss + 1);

      cancelMove();

      playedMoves++;

      if (rootNode)
        rootMoves.scores[rootMoves.indexOf(move)] = value;

      if (value > bestValue) {
        bestValue = value;

        if (bestValue > alpha) {
          bestMove = move;

          // Always true in NonPV nodes
          if (bestValue >= beta)
            break;

          alpha = bestValue;

          updatePV(ss, bestMove);
        }
      }
    }

    if (!foundLegalMove)
      return position.checkers ? Value(ply - VALUE_MATE) : VALUE_DRAW;

    // Update histories
    if (bestMove &&
      position.isQuiet(bestMove))
    {
      int bonus = (bestValue > beta + 150) ? stat_bonus(depth + 1) : stat_bonus(depth);

      int& history = mainHistory[position.sideToMove][fromTo(bestMove)];

      history = myClamp(history + bonus, -12000, +12000);

      if (bestMove != ss->killers[0]) {
        ss->killers[1] = ss->killers[0];
        ss->killers[0] = bestMove;
      }
    }

    // Store to TT
    TT::Flag flag;
    if (bestValue >= beta)
      flag = TT::FLAG_LOWER;
    else
      flag = (PvNode && bestMove) ? TT::FLAG_EXACT : TT::FLAG_UPPER;

    ttEntry->store(position.key, flag, depth, bestMove, bestValue, ss->staticEval);

    return bestValue;
  }


  std::string getPvString(SearchInfo* ss) {

    ostringstream output;

    for (int i = 0; i < ss->pvLength; i++) {
      Move move = ss->pv[i];
      if (!move)
        break;

      output << UCI::move(move) << ' ';
    }

    return output.str();
  }

  constexpr int SsOffset = 4;

  SearchInfo searchStack[MAX_PLY + SsOffset];

  void startSearch() {

    Move bestMove;

    clock_t optimumTime;

    if (searchLimits.hasTimeLimit())
      optimumTime = TimeMan::calcOptimumTime(searchLimits, position.sideToMove);

    for (int c = WHITE; c <= BLACK; c++) {
      for (int i = 0; i < 4096; i++) {
        mainHistory[c][i] /= 5;
      }
    }

    ply = 0;

    nodesSearched = 0;

    rootColor = position.sideToMove;

    SearchLoopInfo iterDeepening[MAX_PLY];

    for (int i = 0; i < MAX_PLY + SsOffset; i++) {
      searchStack[i].staticEval = VALUE_NONE;

      searchStack[i].pvLength = 0;

      searchStack[i].killers[0] = MOVE_NONE;
      searchStack[i].killers[1] = MOVE_NONE;
    }

    SearchInfo* ss = &searchStack[SsOffset];

    if (searchLimits.depth == 0)
      searchLimits.depth = MAX_PLY;

    // Setup root moves
    rootMoves = MoveList();
    {
      MoveList pseudoRootMoves;
      getPseudoLegalMoves(position, &pseudoRootMoves);

      for (int i = 0; i < pseudoRootMoves.size(); i++) {
        Move move = pseudoRootMoves[i];
        if (!position.isLegal(move))
          continue;

        rootMoves.add(move);
      }
    }
    scoreMoves(rootMoves, MOVE_NONE, ss);

    clock_t startTime = clock();

    int searchStability = 0;

    for (rootDepth = 1; rootDepth <= searchLimits.depth; rootDepth++) {

      if (searchLimits.nodes && nodesSearched >= searchLimits.nodes)
        break;

      selDepth = 0;

      Value score;
      if (rootDepth >= 4) {
        int windowSize = 10;
        Value alpha = iterDeepening[rootDepth - 1].score - windowSize;
        Value beta = iterDeepening[rootDepth - 1].score + windowSize;

        int failedHighCnt = 0;
        while (true) {

          int adjustedDepth = myMax(1, rootDepth - failedHighCnt);

          score = negaMax<Root>(alpha, beta, adjustedDepth, false, ss);

          if (Threads::searchState == STOP_PENDING)
            goto bestMoveDecided;

          if (searchLimits.nodes && nodesSearched >= searchLimits.nodes)
            break; // only break, in order to print info about the partial search we've done

          if (score >= VALUE_MATE_IN_MAX_PLY) {
            beta = VALUE_INFINITE;
            failedHighCnt = 0;
          }

          if (score <= alpha) {
            beta = Value((alpha + beta) / 2);
            alpha = (Value)myMax(-VALUE_INFINITE, alpha - windowSize);

            failedHighCnt = 0;
          }
          else if (score >= beta) {
            beta = (Value)myMin(VALUE_INFINITE, beta + windowSize);
            ++failedHighCnt;
          }
          else
            break;

          windowSize += windowSize / 3;
        }
      }
      else {
        score = negaMax<Root>(-VALUE_INFINITE, VALUE_INFINITE, rootDepth, false, ss);
      }

      // It's super important to not update the best move if the search was abruptly stopped
      if (Threads::searchState == STOP_PENDING)
        goto bestMoveDecided;

      iterDeepening[rootDepth].selDepth = selDepth;
      iterDeepening[rootDepth].score = score;
      iterDeepening[rootDepth].bestMove = bestMove = ss->pv[0];

      clock_t elapsed = elapsedTime();

      if (printingEnabled) {
        ostringstream infoStr;
        infoStr
          << "info"
          << " depth " << rootDepth
          << " seldepth " << selDepth
          << " score " << UCI::value(score)
          << " nodes " << nodesSearched
          << " nps " << (nodesSearched * 1000ULL) / std::max(int(elapsed), 1)
          << " time " << elapsed
          << " pv " << getPvString(ss);
        cout << infoStr.str() << endl;
      }

      if (bestMove == iterDeepening[rootDepth - 1].bestMove)
        searchStability = std::min(searchStability + 1, 10);
      else
        searchStability = 0;

      // Stop searching if we can deliver a forced checkmate.
      // No need to stop if we are getting checkmated, instead keep searching,
      // because we may have overlooked a way out of checkmate due to pruning
      if (score >= VALUE_MATE_IN_MAX_PLY)
        goto bestMoveDecided;

      if (searchLimits.hasTimeLimit() && rootDepth >= 4) {

        // If the position is a dead draw, stop searching
        if (rootDepth >= 40 && abs(score) < 5) {
          goto bestMoveDecided;
        }

        double optScale = 1.0 - 0.05 * searchStability;

        if (elapsed > optScale * optimumTime)
          goto bestMoveDecided;
      }
    }

  bestMoveDecided:

    lastBestMove = bestMove;
    lastSearchTimeSpan = clock() - startTime;

    if (printingEnabled)
      std::cout << "bestmove " << UCI::move(bestMove) << endl;

    Threads::searchState = STOPPED;
  }

  void* idleLoop(void*) {
    while (true) {

      while (Threads::searchState != RUNNING) {
        _sleep(1);
      }

      startSearch();
    }
  }
}
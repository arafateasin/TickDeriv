using namespace QPI;

constexpr uint32 ROUND_DURATION = 20;
constexpr uint64 MIN_BET = 1000000;
constexpr uint64 MAX_BET = 1000000000;
constexpr uint32 MAX_BETS_PER_ROUND = 128;
constexpr uint32 HISTORY_SIZE = 8;
constexpr uint32 HOUSE_FEE_BPS = 200;
constexpr uint32 BASIS_POINTS = 10000;

namespace RoundState
{
    constexpr uint8 PENDING = 0;
    constexpr uint8 ACTIVE = 1;
    constexpr uint8 LOCKED = 2;
    constexpr uint8 COMPLETED = 3;
}

namespace Direction
{
    constexpr uint8 DOWN = 0;
    constexpr uint8 UP = 1;
}

struct Round
{
    uint32 id;
    uint32 startTick;
    uint32 endTick;
    uint32 lockTick;
    sint64 startPrice;
    sint64 endPrice;
    uint64 poolUp;
    uint64 poolDown;
    uint64 totalPayout;
    uint8 state;
    uint8 winningDirection;
    uint32 betCount;
};

struct BetRecord
{
    id bettor;
    uint32 roundId;
    uint64 amount;
    uint64 payout;
    uint8 direction;
    bit claimed;
    bit won;
    uint32 timestamp;
};

struct TickDerivState
{
    Round currentRound;
    Array<Round, HISTORY_SIZE> history;
    uint32 historyWriteIndex;
    uint32 totalRoundsCount;
    uint64 totalVolumeAllTime;
    uint64 totalPayoutsAllTime;
    Array<BetRecord, MAX_BETS_PER_ROUND> currentBets;
    uint32 currentBetCount;
    Array<BetRecord, 1024> allBets;
    uint32 allBetsCount;
    id owner;
    uint64 collectedFees;
};

struct TickDerivState2
{
};

struct TickDeriv : public ContractBase
{
public:
    struct PlaceBet_input
    {
        uint8 direction;
    };

    typedef NoData PlaceBet_output;

    PUBLIC_PROCEDURE(PlaceBet)
    {
        if (state.currentRound.state != RoundState::ACTIVE)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward());
            return;
        }

        if (input.direction != Direction::DOWN && input.direction != Direction::UP)
        {
            qpi.transfer(qpi.invocator(), qpi.invocationReward());
            return;
        }

        uint64 betAmount = qpi.invocationReward();

        if (betAmount < MIN_BET || betAmount > MAX_BET)
        {
            qpi.transfer(qpi.invocator(), betAmount);
            return;
        }

        if (state.currentBetCount >= MAX_BETS_PER_ROUND)
        {
            qpi.transfer(qpi.invocator(), betAmount);
            return;
        }

        BetRecord bet;
        bet.bettor = qpi.invocator();
        bet.roundId = state.currentRound.id;
        bet.amount = betAmount;
        bet.direction = input.direction;
        bet.claimed = false;
        bet.won = false;
        bet.payout = 0;
        bet.timestamp = qpi.tick();

        state.currentBets.set(state.currentBetCount, bet);
        state.currentBetCount++;
        state.currentRound.betCount++;

        if (state.allBetsCount < 1024)
        {
            state.allBets.set(state.allBetsCount, bet);
            state.allBetsCount++;
        }

        if (input.direction == Direction::UP)
        {
            state.currentRound.poolUp = state.currentRound.poolUp + betAmount;
        }
        else
        {
            state.currentRound.poolDown = state.currentRound.poolDown + betAmount;
        }

        state.totalVolumeAllTime = state.totalVolumeAllTime + betAmount;
    }

    typedef NoData ResolveRound_input;

    struct ResolveRound_output
    {
        bit resolved;
        uint8 winningDirection;
        uint64 totalPayout;
        sint64 startPrice;
        sint64 endPrice;
    };

    PUBLIC_PROCEDURE(ResolveRound)
    {
        output.resolved = false;
        output.winningDirection = 2;
        output.totalPayout = 0;
        output.startPrice = state.currentRound.startPrice;
        output.endPrice = 0;

        if (state.currentRound.state != RoundState::LOCKED)
        {
            return;
        }

        uint32 currentTick = qpi.tick();
        if (currentTick <= state.currentRound.endTick)
        {
            return;
        }

        uint64 seedEnd = (uint64)currentTick;
        seedEnd = seedEnd * 1103515245ULL + 12345ULL;
        seedEnd = (seedEnd / 65536ULL) % 32768ULL;
        sint64 basePriceEnd = 2500;
        sint64 varianceEnd = (sint64)(seedEnd % 1000) - 500;
        sint64 priceEnd = basePriceEnd + varianceEnd;
        state.currentRound.endPrice = priceEnd * 100000;
        output.endPrice = state.currentRound.endPrice;

        if (state.currentRound.endPrice > state.currentRound.startPrice)
        {
            state.currentRound.winningDirection = Direction::UP;
        }
        else if (state.currentRound.endPrice < state.currentRound.startPrice)
        {
            state.currentRound.winningDirection = Direction::DOWN;
        }
        else
        {
            state.currentRound.winningDirection = 2;
        }

        output.winningDirection = state.currentRound.winningDirection;

        uint64 totalPool = state.currentRound.poolUp + state.currentRound.poolDown;

        if (state.currentRound.winningDirection == 2)
        {
            for (uint32 i = 0; i < state.currentBetCount; ++i)
            {
                BetRecord bet = state.currentBets.get(i);
                output.totalPayout = output.totalPayout + bet.amount;

                bet.payout = bet.amount;
                bet.claimed = false;
                bet.won = true;
                state.currentBets.set(i, bet);

                for (uint32 j = 0; j < state.allBetsCount; ++j)
                {
                    BetRecord storedBet = state.allBets.get(j);
                    if (storedBet.bettor == bet.bettor && storedBet.roundId == bet.roundId && storedBet.timestamp == bet.timestamp)
                    {
                        state.allBets.set(j, bet);
                        break;
                    }
                }
            }
        }
        else
        {
            uint64 houseFee = (totalPool * HOUSE_FEE_BPS) / BASIS_POINTS;
            uint64 poolAfterFee = totalPool - houseFee;
            state.collectedFees = state.collectedFees + houseFee;

            uint64 winningPool = (state.currentRound.winningDirection == Direction::UP)
                ? state.currentRound.poolUp
                : state.currentRound.poolDown;

            for (uint32 i = 0; i < state.currentBetCount; ++i)
            {
                BetRecord bet = state.currentBets.get(i);

                if (bet.direction == state.currentRound.winningDirection)
                {
                    if (winningPool > 0)
                    {
                        uint64 payout = (poolAfterFee * bet.amount) / winningPool;
                        output.totalPayout = output.totalPayout + payout;

                        bet.payout = payout;
                        bet.claimed = false;
                        bet.won = true;
                    }
                }
                else
                {
                    bet.won = false;
                    bet.payout = 0;
                    bet.claimed = true;
                }

                state.currentBets.set(i, bet);

                for (uint32 j = 0; j < state.allBetsCount; ++j)
                {
                    BetRecord storedBet = state.allBets.get(j);
                    if (storedBet.bettor == bet.bettor && storedBet.roundId == bet.roundId && storedBet.timestamp == bet.timestamp)
                    {
                        state.allBets.set(j, bet);
                        break;
                    }
                }
            }
        }

        state.currentRound.state = RoundState::COMPLETED;
        state.currentRound.totalPayout = output.totalPayout;
        state.totalPayoutsAllTime = state.totalPayoutsAllTime + output.totalPayout;
        output.resolved = true;

        state.history.set(state.historyWriteIndex & (HISTORY_SIZE - 1), state.currentRound);
        state.historyWriteIndex++;
        state.totalRoundsCount++;

        uint32 newTick = qpi.tick();
        state.currentRound.id = state.totalRoundsCount + 1;
        state.currentRound.startTick = newTick;
        state.currentRound.endTick = newTick + ROUND_DURATION;
        state.currentRound.lockTick = 0;
        
        uint64 seedNewRound = (uint64)newTick;
        seedNewRound = seedNewRound * 1103515245ULL + 12345ULL;
        seedNewRound = (seedNewRound / 65536ULL) % 32768ULL;
        sint64 basePriceNewRound = 2500;
        sint64 varianceNewRound = (sint64)(seedNewRound % 1000) - 500;
        sint64 priceNewRound = basePriceNewRound + varianceNewRound;
        state.currentRound.startPrice = priceNewRound * 100000;
        
        state.currentRound.endPrice = 0;
        state.currentRound.poolUp = 0;
        state.currentRound.poolDown = 0;
        state.currentRound.totalPayout = 0;
        state.currentRound.state = RoundState::ACTIVE;
        state.currentRound.winningDirection = 2;
        state.currentRound.betCount = 0;
        state.currentBetCount = 0;
    }

    typedef NoData GetCurrentRound_input;

    struct GetCurrentRound_output
    {
        Round round;
        uint32 currentTick;
        uint32 betCount;
    };

    PUBLIC_FUNCTION(GetCurrentRound)
    {
        output.round = state.currentRound;
        output.currentTick = qpi.tick();
        output.betCount = state.currentBetCount;
    }

    typedef NoData GetRoundHistory_input;

    struct GetRoundHistory_output
    {
        Array<Round, HISTORY_SIZE> history;
        uint32 totalRoundsCount;
        uint32 historySize;
    };

    PUBLIC_FUNCTION(GetRoundHistory)
    {
        output.history = state.history;
        output.totalRoundsCount = state.totalRoundsCount;
        output.historySize = (state.totalRoundsCount < HISTORY_SIZE)
            ? state.totalRoundsCount
            : HISTORY_SIZE;
    }

    struct GetUserBets_input
    {
        id userAddress;
    };

    struct GetUserBets_output
    {
        Array<BetRecord, MAX_BETS_PER_ROUND> bets;
        uint32 betCount;
    };

    PUBLIC_FUNCTION(GetUserBets)
    {
        output.betCount = 0;

        for (uint32 i = 0; i < state.allBetsCount && output.betCount < MAX_BETS_PER_ROUND; ++i)
        {
            BetRecord bet = state.allBets.get(i);
            if (bet.bettor == input.userAddress)
            {
                output.bets.set(output.betCount, bet);
                output.betCount++;
            }
        }
    }

    typedef NoData GetContractStats_input;

    struct GetContractStats_output
    {
        uint32 totalRounds;
        uint64 totalVolume;
        uint64 totalPayouts;
        uint64 collectedFees;
        uint32 currentRoundId;
        uint32 currentTick;
    };

    PUBLIC_FUNCTION(GetContractStats)
    {
        output.totalRounds = state.totalRoundsCount;
        output.totalVolume = state.totalVolumeAllTime;
        output.totalPayouts = state.totalPayoutsAllTime;
        output.collectedFees = state.collectedFees;
        output.currentRoundId = state.currentRound.id;
        output.currentTick = qpi.tick();
    }

    typedef NoData WithdrawFees_input;

    struct WithdrawFees_output
    {
        uint64 amount;
        bit success;
    };

    PUBLIC_PROCEDURE(WithdrawFees)
    {
        output.amount = 0;
        output.success = false;

        if (qpi.invocator() != state.owner)
        {
            return;
        }

        if (state.collectedFees > 0)
        {
            qpi.transfer(state.owner, state.collectedFees);
            output.amount = state.collectedFees;
            output.success = true;
            state.collectedFees = 0;
        }
    }

    typedef NoData ClaimWinnings_input;

    struct ClaimWinnings_output
    {
        uint64 totalClaimed;
        uint32 betsClaimed;
        bit success;
    };

    PUBLIC_PROCEDURE(ClaimWinnings)
    {
        output.totalClaimed = 0;
        output.betsClaimed = 0;
        output.success = false;

        id claimer = qpi.invocator();

        for (uint32 i = 0; i < state.allBetsCount; ++i)
        {
            BetRecord bet = state.allBets.get(i);

            if (bet.bettor == claimer && !bet.claimed && bet.won && bet.payout > 0)
            {
                qpi.transfer(claimer, bet.payout);
                output.totalClaimed = output.totalClaimed + bet.payout;
                output.betsClaimed++;

                bet.claimed = true;
                state.allBets.set(i, bet);
            }
        }

        if (output.totalClaimed > 0)
        {
            output.success = true;
        }
    }

    struct GetUserClaimable_input
    {
        id userAddress;
    };

    struct GetUserClaimable_output
    {
        uint64 totalClaimable;
        uint32 unclaimedBets;
    };

    PUBLIC_FUNCTION(GetUserClaimable)
    {
        output.totalClaimable = 0;
        output.unclaimedBets = 0;

        for (uint32 i = 0; i < state.allBetsCount; ++i)
        {
            BetRecord bet = state.allBets.get(i);

            if (bet.bettor == input.userAddress && !bet.claimed && bet.won && bet.payout > 0)
            {
                output.totalClaimable = output.totalClaimable + bet.payout;
                output.unclaimedBets++;
            }
        }
    }

    REGISTER_USER_FUNCTIONS_AND_PROCEDURES()
    {
        REGISTER_USER_PROCEDURE(PlaceBet, 1);
        REGISTER_USER_PROCEDURE(ResolveRound, 2);
        REGISTER_USER_PROCEDURE(WithdrawFees, 3);
        REGISTER_USER_PROCEDURE(ClaimWinnings, 4);
        REGISTER_USER_FUNCTION(GetCurrentRound, 1);
        REGISTER_USER_FUNCTION(GetRoundHistory, 2);
        REGISTER_USER_FUNCTION(GetUserBets, 3);
        REGISTER_USER_FUNCTION(GetContractStats, 4);
        REGISTER_USER_FUNCTION(GetUserClaimable, 5);
    }

    INITIALIZE()
    {
        state.owner = qpi.invocator();
        state.totalRoundsCount = 0;
        state.totalVolumeAllTime = 0;
        state.totalPayoutsAllTime = 0;
        state.collectedFees = 0;
        state.historyWriteIndex = 0;
        state.currentBetCount = 0;
        state.allBetsCount = 0;

        for (uint32 i = 0; i < HISTORY_SIZE; ++i)
        {
            Round emptyRound;
            emptyRound.id = 0;
            emptyRound.state = RoundState::COMPLETED;
            emptyRound.poolUp = 0;
            emptyRound.poolDown = 0;
            state.history.set(i, emptyRound);
        }

        uint32 initTick = qpi.tick();
        state.currentRound.id = state.totalRoundsCount + 1;
        state.currentRound.startTick = initTick;
        state.currentRound.endTick = initTick + ROUND_DURATION;
        state.currentRound.lockTick = 0;
        
        uint64 seedInit = (uint64)initTick;
        seedInit = seedInit * 1103515245ULL + 12345ULL;
        seedInit = (seedInit / 65536ULL) % 32768ULL;
        sint64 basePriceInit = 2500;
        sint64 varianceInit = (sint64)(seedInit % 1000) - 500;
        sint64 priceInit = basePriceInit + varianceInit;
        state.currentRound.startPrice = priceInit * 100000;
        
        state.currentRound.endPrice = 0;
        state.currentRound.poolUp = 0;
        state.currentRound.poolDown = 0;
        state.currentRound.totalPayout = 0;
        state.currentRound.state = RoundState::ACTIVE;
        state.currentRound.winningDirection = 2;
        state.currentRound.betCount = 0;
        state.currentBetCount = 0;
    }

    BEGIN_TICK()
    {
        uint32 currentTick = qpi.tick();

        if (state.currentRound.state == RoundState::ACTIVE)
        {
            if (currentTick >= state.currentRound.endTick)
            {
                state.currentRound.state = RoundState::LOCKED;
                state.currentRound.lockTick = currentTick;
            }
        }

        if (state.currentRound.state == RoundState::LOCKED)
        {
            if (currentTick >= state.currentRound.endTick + 5)
            {
                if (state.currentRound.state == RoundState::LOCKED)
                {
                    uint32 currentTick = qpi.tick();

                    uint64 seed = (uint64)currentTick;
                    seed = seed * 1103515245ULL + 12345ULL;
                    seed = (seed / 65536ULL) % 32768ULL;
                    sint64 basePrice = 2500;
                    sint64 variance = (sint64)(seed % 1000) - 500;
                    sint64 price = basePrice + variance;
                    state.currentRound.endPrice = price * 100000;

                    if (state.currentRound.endPrice > state.currentRound.startPrice)
                    {
                        state.currentRound.winningDirection = Direction::UP;
                    }
                    else if (state.currentRound.endPrice < state.currentRound.startPrice)
                    {
                        state.currentRound.winningDirection = Direction::DOWN;
                    }
                    else
                    {
                        state.currentRound.winningDirection = 2;
                    }

                    uint64 totalPool = state.currentRound.poolUp + state.currentRound.poolDown;
                    uint64 totalPayout = 0;

                    if (state.currentRound.winningDirection == 2)
                    {
                        for (uint32 i = 0; i < state.currentBetCount; ++i)
                        {
                            BetRecord bet = state.currentBets.get(i);
                            totalPayout = totalPayout + bet.amount;
                            bet.payout = bet.amount;
                            bet.claimed = false;
                            bet.won = true;
                            state.currentBets.set(i, bet);

                            for (uint32 j = 0; j < state.allBetsCount; ++j)
                            {
                                BetRecord storedBet = state.allBets.get(j);
                                if (storedBet.bettor == bet.bettor && storedBet.roundId == bet.roundId && storedBet.timestamp == bet.timestamp)
                                {
                                    state.allBets.set(j, bet);
                                    break;
                                }
                            }
                        }
                    }
                    else
                    {
                        uint64 houseFee = (totalPool * HOUSE_FEE_BPS) / BASIS_POINTS;
                        uint64 poolAfterFee = totalPool - houseFee;
                        state.collectedFees = state.collectedFees + houseFee;

                        uint64 winningPool = (state.currentRound.winningDirection == Direction::UP)
                            ? state.currentRound.poolUp
                            : state.currentRound.poolDown;

                        for (uint32 i = 0; i < state.currentBetCount; ++i)
                        {
                            BetRecord bet = state.currentBets.get(i);

                            if (bet.direction == state.currentRound.winningDirection)
                            {
                                if (winningPool > 0)
                                {
                                    uint64 payout = (poolAfterFee * bet.amount) / winningPool;
                                    totalPayout = totalPayout + payout;
                                    bet.payout = payout;
                                    bet.claimed = false;
                                    bet.won = true;
                                }
                            }
                            else
                            {
                                bet.won = false;
                                bet.payout = 0;
                                bet.claimed = true;
                            }

                            state.currentBets.set(i, bet);

                            for (uint32 j = 0; j < state.allBetsCount; ++j)
                            {
                                BetRecord storedBet = state.allBets.get(j);
                                if (storedBet.bettor == bet.bettor && storedBet.roundId == bet.roundId && storedBet.timestamp == bet.timestamp)
                                {
                                    state.allBets.set(j, bet);
                                    break;
                                }
                            }
                        }
                    }

                    state.currentRound.state = RoundState::COMPLETED;
                    state.currentRound.totalPayout = totalPayout;
                    state.totalPayoutsAllTime = state.totalPayoutsAllTime + totalPayout;

                    state.history.set(state.historyWriteIndex & (HISTORY_SIZE - 1), state.currentRound);
                    state.historyWriteIndex++;
                    state.totalRoundsCount++;

                    uint32 newTick = qpi.tick();
                    state.currentRound.id = state.totalRoundsCount + 1;
                    state.currentRound.startTick = newTick;
                    state.currentRound.endTick = newTick + ROUND_DURATION;
                    state.currentRound.lockTick = 0;
                    
                    uint64 seedStart = (uint64)newTick;
                    seedStart = seedStart * 1103515245ULL + 12345ULL;
                    seedStart = (seedStart / 65536ULL) % 32768ULL;
                    sint64 basePriceStart = 2500;
                    sint64 varianceStart = (sint64)(seedStart % 1000) - 500;
                    sint64 priceStart = basePriceStart + varianceStart;
                    state.currentRound.startPrice = priceStart * 100000;
                    
                    state.currentRound.endPrice = 0;
                    state.currentRound.poolUp = 0;
                    state.currentRound.poolDown = 0;
                    state.currentRound.totalPayout = 0;
                    state.currentRound.state = RoundState::ACTIVE;
                    state.currentRound.winningDirection = 2;
                    state.currentRound.betCount = 0;
                    state.currentBetCount = 0;
                }
            }
        }
    }
};

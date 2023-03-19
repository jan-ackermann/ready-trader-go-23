// Copyright 2021 Optiver Asia Pacific Pty. Ltd.
//
// This file is part of Ready Trader Go.
//
//     Ready Trader Go is free software: you can redistribute it and/or
//     modify it under the terms of the GNU Affero General Public License
//     as published by the Free Software Foundation, either version 3 of
//     the License, or (at your option) any later version.
//
//     Ready Trader Go is distributed in the hope that it will be useful,
//     but WITHOUT ANY WARRANTY; without even the implied warranty of
//     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//     GNU Affero General Public License for more details.
//
//     You should have received a copy of the GNU Affero General Public
//     License along with Ready Trader Go.  If not, see
//     <https://www.gnu.org/licenses/>.
#include <array>

#include <boost/asio/io_context.hpp>

#include <cmath>

#include "autotrader.h"

using namespace ReadyTraderGo;

time_duration MessageFrequencyTracker::PeriodLength = boost::posix_time::seconds(1);

AutoTrader::AutoTrader(boost::asio::io_context& context) : BaseAutoTrader(context)
{
}

void AutoTrader::DisconnectHandler()
{
    BaseAutoTrader::DisconnectHandler();
    RLOG(LG_AT, LogLevel::LL_INFO) << "execution connection lost";
}

void AutoTrader::ErrorMessageHandler(unsigned long clientOrderId,
                                     const std::string& errorMessage)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "error with order " << clientOrderId << ": " << errorMessage;
    if (clientOrderId != 0 && ((mAskOrderIdToOrder.count(clientOrderId) == 1) || (mBidOrderIdToOrder.count(clientOrderId) == 1)))
    {
        OrderStatusMessageHandler(clientOrderId, 0, 0, 0);
    }
}

void AutoTrader::HedgeFilledMessageHandler(unsigned long clientOrderId,
                                           unsigned long price,
                                           unsigned long volume)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "hedge order " << clientOrderId << " filled for " << volume
                                   << " lots at $" << price << " average price in cents";
}

void AutoTrader::OrderBookMessageHandler(Instrument instrument,
                                         unsigned long sequenceNumber,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& askPrices,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& askVolumes,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& bidPrices,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT>& bidVolumes)
{
    if (instrument == FUT) {
        unsigned long bestBidFut = bidPrices[0];
        unsigned long bestBidVolFut = bidVolumes[0];
        unsigned long bestAskFut = askPrices[0];
        unsigned long bestAskVolFut = askVolumes[0];

        // Get out of bad orders right away
        // Cancel arbitragable bid orders
        //std::array<unsigned long, 32> cancelled_orders {};
        //unsigned int index = 0;
        for (unsigned long price : mBidPrices) {
            if (bestAskFut < price) {
                unsigned int orderId = mBidToOrder[price]->orderId;
                SendCancelOrder(orderId);
                RLOG(LG_AT, LogLevel::LL_INFO) << "cancelling order " << orderId << " at price " << price;
                //cancelled_orders[index++] = orderId;
            }
        }
        // Cancel arbitragable ask orders
        for (unsigned long price : mAskPrices) {
            if (bestBidFut > price) {
                unsigned int orderId = mAskToOrder[price]->orderId;
                SendCancelOrder(orderId);
                RLOG(LG_AT, LogLevel::LL_INFO) << "cancelling order " << orderId << " at price " << price;
                //cancelled_orders[index++] = orderId;
            }
        }

        // Proper market making code
        // Minimum position imbalance before a price adjustment can be made
        constexpr long minPositionImbalance = 50;
        // @1.0 = ; @0.5 = ; @0.25 = ; @0.0 =
        constexpr double centsPerImbalancedShare = 1.0 / LOT_SIZE;
        unsigned long priceAdjustment = 0;
        if (mPosition >= minPositionImbalance) {
            priceAdjustment = -(int)round(((double)mPosition - (double)minPositionImbalance) * centsPerImbalancedShare);
        } else if (mPosition <= -minPositionImbalance) {
            priceAdjustment = -(int)round(((double)mPosition + (double)minPositionImbalance) * centsPerImbalancedShare);
        }
        priceAdjustment *= TICK_SIZE_IN_CENTS;

        int numNewOrdersAllowed = mMessageTracker.GetNonCancelMessagesAllowed();

        // Adjust bid side
        if (bestBidFut != 0) {
            // Calculate front of my book bid
            unsigned long frontBid = bestBidFut + priceAdjustment - ADDITIONAL_SPREAD;
            frontBid = std::min(frontBid, bestAskFut);
            // Count to compute the maximum bid size
            // and also mark orders for cancellation that are too far away
            long maximumBidSize = POSITION_LIMIT - mPosition;
            for (unsigned int price : mBidPrices) {
                Order* order = mBidToOrder[price];
                // Backstop in case mBidPrices, mBidToOrder, and mBidOrderIdToOrder become inconsistent
                if (order == nullptr)
                    continue;

                /*if (mBidToOrder.count(price) == 0) {
                    RLOG(LG_AT, LogLevel::LL_ERROR) << "mBidPrices and mBidToOrder is inconsistent for price " << price;
                    RLOG(LG_AT, LogLevel::LL_INFO) << "mBidPrices:";
                    for (auto& p : mBidPrices) RLOG(LG_AT, LogLevel::LL_INFO) << p;
                    RLOG(LG_AT, LogLevel::LL_INFO) << "mBidToOrder:";
                    for (auto& p : mBidToOrder) RLOG(LG_AT, LogLevel::LL_INFO) << p.first << " maps to " << p.second;
                    RLOG(LG_AT, LogLevel::LL_INFO) << "mBidOrderIdToOrder:";
                    for (auto& p : mBidOrderIdToOrder) RLOG(LG_AT, LogLevel::LL_INFO) << p.first << " maps to " << p.second;
                    RLOG(LG_AT, LogLevel::LL_INFO) << std::flush;
                    return;
                }*/

                if (price > frontBid || price <= frontBid - NUM_CLONES * TICK_SIZE_IN_CENTS) {
                    SendCancelOrder(order->orderId);
                    // This order will be cancelled, but it will contribute still
                    // towards reducing the maximum bid size, because it might be
                    // filled before the cancellation is effective
                    maximumBidSize -= (long)order->volume;
                } else {
                    // Order is fine
                    maximumBidSize -= (long)order->volume;
                }
            }
            for (unsigned int offset = 0; offset < NUM_CLONES && maximumBidSize > 0 && numNewOrdersAllowed > 0; offset++) {
                unsigned int price = frontBid - offset * TICK_SIZE_IN_CENTS;
                if (mBidPrices.count(price) == 0) {
                    unsigned int volume = std::min((long)LOT_SIZE, maximumBidSize);
                    unsigned int orderId = mNextMessageId++;
                    SendInsertOrder(orderId, Side::BUY, price, volume, Lifespan::GOOD_FOR_DAY);
                    numNewOrdersAllowed--;
                    auto* order = new Order(price, volume, orderId);
                    mBidPrices.emplace(price);
                    mBidToOrder[price] = order;
                    mBidOrderIdToOrder[orderId] = order;
                    maximumBidSize -= volume;
                }
            }
        }

        // Adjust ask side
        if (bestAskFut != 0) {
            // Calculate front of my book ask
            unsigned long frontAsk = bestAskFut + priceAdjustment + ADDITIONAL_SPREAD;
            frontAsk = std::max(frontAsk, bestBidFut);
            // Count to compute the maximum ask size
            // and also mark orders for cancellation that are too far away
            long maximumAskSize = POSITION_LIMIT + mPosition;
            for (unsigned int price : mAskPrices) {
                Order* order = mAskToOrder[price];
                // Backstop in case mAskPrices, mAskToOrder, and mAskOrderIdToOrder become inconsistent
                if (order == nullptr)
                    continue;

                /*if (mAskToOrder.count(price) == 0) {
                    RLOG(LG_AT, LogLevel::LL_ERROR) << "mAskPrices and mAskToOrder is inconsistent for price " << price;
                    RLOG(LG_AT, LogLevel::LL_INFO) << "mAskPrices:";
                    for (auto& p : mAskPrices) RLOG(LG_AT, LogLevel::LL_INFO) << p;
                    RLOG(LG_AT, LogLevel::LL_INFO) << "mAskToOrder:";
                    for (auto& p : mAskToOrder) RLOG(LG_AT, LogLevel::LL_INFO) << p.first << " maps to " << p.second;
                    RLOG(LG_AT, LogLevel::LL_INFO) << "mAskOrderIdToOrder:";
                    for (auto& p : mAskOrderIdToOrder) RLOG(LG_AT, LogLevel::LL_INFO) << p.first << " maps to " << p.second;
                    RLOG(LG_AT, LogLevel::LL_INFO) << std::flush;
                    return;
                }*/

                if (price < frontAsk || price >= frontAsk + NUM_CLONES * TICK_SIZE_IN_CENTS) {
                    SendCancelOrder(order->orderId);
                    // This order will be cancelled, but it will contribute still
                    // towards reducing the maximum ask size, because it might be
                    // filled before the cancellation is effective
                    maximumAskSize -= (long)order->volume;
                } else {
                    // Order is fine
                    maximumAskSize -= (long)order->volume;
                }
            }
            for (unsigned int offset = 0; offset < NUM_CLONES && maximumAskSize > 0 && numNewOrdersAllowed > 0; offset++) {
                unsigned int price = frontAsk + offset * TICK_SIZE_IN_CENTS;
                if (mAskPrices.count(price) == 0) {
                    unsigned int volume = std::min((long)LOT_SIZE, maximumAskSize);
                    unsigned int orderId = mNextMessageId++;
                    SendInsertOrder(orderId, Side::SELL, price, volume, Lifespan::GOOD_FOR_DAY);
                    numNewOrdersAllowed--;
                    auto* order = new Order(price, volume, orderId);
                    mAskPrices.emplace(price);
                    mAskToOrder[price] = order;
                    mAskOrderIdToOrder[orderId] = order;
                    maximumAskSize -= volume;
                }
            }
        }
    }

    RLOG(LG_AT, LogLevel::LL_INFO) << "order book received for " << instrument << " instrument"
                                   << ": ask prices: " << askPrices[0]
                                   << "; ask volumes: " << askVolumes[0]
                                   << "; bid prices: " << bidPrices[0]
                                   << "; bid volumes: " << bidVolumes[0];

    unsigned long bidQuote = mBidPrices.empty() ? 0 : *mBidPrices.crbegin();
    unsigned long askQuote = mAskPrices.empty() ? 0 : *mAskPrices.cbegin();
    RLOG(LG_AT, LogLevel::LL_INFO) << "making market for ETF " << bidQuote << ":" << askQuote;
}

void AutoTrader::OrderFilledMessageHandler(unsigned long clientOrderId,
                                           unsigned long price,
                                           unsigned long volume)
{
    if (mBidOrderIdToOrder.count(clientOrderId) != 0) {
        SendHedgeOrder(mNextMessageId, Side::SELL, MIN_BID_NEAREST_TICK, volume);
        mNextMessageId++;
        mPosition += (long)volume;
    } else { // if (mAsks.count(clientOrderId) != 0) {
        SendHedgeOrder(mNextMessageId, Side::BUY, MAX_ASK_NEAREST_TICK, volume);
        mNextMessageId++;
        mPosition -= (long)volume;
    }

    RLOG(LG_AT, LogLevel::LL_INFO) << "order " << clientOrderId << " filled for " << volume
                                   << " lots at $" << price << " cents";
}

void AutoTrader::OrderStatusMessageHandler(unsigned long clientOrderId,
                                           unsigned long fillVolume,
                                           unsigned long remainingVolume,
                                           signed long fees)
{
    if (mBidOrderIdToOrder.count(clientOrderId) != 0) {
        if (remainingVolume == 0) {
            Order* order = mBidOrderIdToOrder[clientOrderId];
            mBidOrderIdToOrder.erase(clientOrderId);
            // Backstop in case mBidPrices, mBidToOrder, and mBidOrderIdToOrder become inconsistent
            if (order == nullptr) {
                RLOG(LG_AT, LogLevel::LL_WARNING) << " found inconsistency within bid side order tracking datastructures";
                return;
            }
            mBidToOrder.erase(order->price);
            mBidPrices.erase(order->price);
            delete order;
        } else {
            mBidOrderIdToOrder[clientOrderId]->volume = remainingVolume;
        }
    } else if (mAskOrderIdToOrder.count(clientOrderId) != 0) {
        if (remainingVolume == 0) {
            Order* order = mAskOrderIdToOrder[clientOrderId];
            mAskOrderIdToOrder.erase(clientOrderId);
            // Backstop in case mAskPrices, mAskToOrder, and mAskOrderIdToOrder become inconsistent
            if (order == nullptr) {
                RLOG(LG_AT, LogLevel::LL_WARNING) << " found inconsistency within ask side order tracking datastructures";
                return;
            }
            mAskToOrder.erase(order->price);
            mAskPrices.erase(order->price);
            delete order;
        } else {
            mAskOrderIdToOrder[clientOrderId]->volume = remainingVolume;
        }
    } else {
        RLOG(LG_AT, LogLevel::LL_WARNING) << "unknown order " << clientOrderId << " had an update!";
    }
}

void AutoTrader::TradeTicksMessageHandler(Instrument instrument,
                                          unsigned long sequenceNumber,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT>& askPrices,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT>& askVolumes,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT>& bidPrices,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT>& bidVolumes)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "trade ticks received for " << instrument << " instrument"
                                   << ": ask prices: " << askPrices[0]
                                   << "; ask volumes: " << askVolumes[0]
                                   << "; bid prices: " << bidPrices[0]
                                   << "; bid volumes: " << bidVolumes[0];
}

void AutoTrader::SendAmendOrder(unsigned long clientOrderId, unsigned long volume)
{
    mMessageTracker.NoteMessage();
    BaseAutoTrader::SendAmendOrder(clientOrderId, volume);
    RLOG(LG_AT, LogLevel::LL_DEBUG) << " sent amend order message";
}

void AutoTrader::SendCancelOrder(unsigned long clientOrderId)
{
    mMessageTracker.NoteMessage();
    BaseAutoTrader::SendCancelOrder(clientOrderId);
    RLOG(LG_AT, LogLevel::LL_DEBUG) << " sent cancel order message";
}

void AutoTrader::SendHedgeOrder(unsigned long clientOrderId, Side side, unsigned long price, unsigned long volume)
{
    mMessageTracker.NoteMessage();
    BaseAutoTrader::SendHedgeOrder(clientOrderId, side, price, volume);
    RLOG(LG_AT, LogLevel::LL_DEBUG) << " sent hedge order message";
}

void AutoTrader::SendInsertOrder(unsigned long clientOrderId, Side side, unsigned long price, unsigned long volume, Lifespan lifespan)
{
    mMessageTracker.NoteMessage();
    BaseAutoTrader::SendInsertOrder(clientOrderId, side, price, volume, lifespan);
    RLOG(LG_AT, LogLevel::LL_DEBUG) << " sent insert order message";
}

void MessageFrequencyTracker::NoteMessage()
{
    // Add new message and advance pointer
    ptime currentTime = boost::posix_time::microsec_clock::universal_time();
    *(mTail++) = currentTime;
    mRollingMessageCount++;
    if (mTail == mMem.end())
        mTail = mMem.begin();

    // Remove timed out messages
    while (mHead != mTail && currentTime - *mHead > MessageFrequencyTracker::PeriodLength) {
        mRollingMessageCount--;
        if (++mHead == mMem.end())
            mHead = mMem.begin();
    }
    RLOG(LG_AT, LogLevel::LL_WARNING) << " rolling message count " << mRollingMessageCount;

    // Wait until submission is safe, this is a safety mechanism
    // and should ideally never be used
    if (mRollingMessageCount > MAX_MESSAGE_FREQ) {
        time_duration wait_for = currentTime - *mHead + boost::posix_time::milliseconds(100);
        RLOG(LG_AT, LogLevel::LL_WARNING) << " before message submission waiting for " << wait_for;
        boost::this_thread::sleep(wait_for);
    }
}

int MessageFrequencyTracker::GetNonCancelMessagesAllowed()
{
    // Remove timed out messages
    ptime currentTime = boost::posix_time::microsec_clock::universal_time();
    while (mHead != mTail && currentTime - *mHead > MessageFrequencyTracker::PeriodLength) {
        mRollingMessageCount--;
        if (++mHead == mMem.end())
            mHead = mMem.begin();
    }
    
    // Figure out what message strategy is guaranteed to be compliant
    constexpr unsigned long safetyMargin = 0;
    constexpr unsigned long maxOpenOrders = 2 * NUM_CLONES;
    unsigned long freeMessages = MAX_MESSAGE_FREQ - mRollingMessageCount;
    return (int)std::max(0UL, (freeMessages - maxOpenOrders - safetyMargin) / 2);
}
// python rtg.py run autotrader
// ./compile.sh
// git pull; ./compile.sh; python rtg.py run autotrader

// Todo - fix own book crossing orders
//      - check what happens if input to HandleOrderBookUpdate is corrupt, no bid or no ask or whatever
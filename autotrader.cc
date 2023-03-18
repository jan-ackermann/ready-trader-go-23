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

RTG_INLINE_GLOBAL_LOGGER_WITH_CHANNEL(LG_AT, "AUTO")

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
        // Get out of bad orders right away
        // Cancel arbitragable bid orders
        //std::array<unsigned long, 32> cancelled_orders {};
        //unsigned int index = 0;
        for (unsigned long price : mBidPrices) {
            if (askPrices[0] < price) {
                unsigned int orderId = mBidToOrder[price]->orderId;
                SendCancelOrder(orderId);
                RLOG(LG_AT, LogLevel::LL_INFO) << "cancelling order " << orderId << " at price " << price;
                //cancelled_orders[index++] = orderId;
            }
        }
        // Cancel arbitragable ask orders
        for (unsigned long price : mAskPrices) {
            if (bidPrices[0] > price) {
                unsigned int orderId = mAskToOrder[price]->orderId;
                SendCancelOrder(orderId);
                RLOG(LG_AT, LogLevel::LL_INFO) << "cancelling order " << orderId << " at price " << price;
                //cancelled_orders[index++] = orderId;
            }
        }

        // Proper market making code
        //unsigned long priceAdjustment = -((int)round((double)mPosition / (3.0 * LOT_SIZE))) * TICK_SIZE_IN_CENTS;
        unsigned long priceAdjustment = 0;
        if (mPosition >= 50) {
            priceAdjustment = -((int)round(((double)mPosition - 50.0) / (2.5 * LOT_SIZE))) * TICK_SIZE_IN_CENTS;
        } else if (mPosition <= -50) {
            priceAdjustment = -((int)round(((double)mPosition + 50.0) / (2.5 * LOT_SIZE))) * TICK_SIZE_IN_CENTS;
        }

        int numNewOrdersAllowed = mMessageTracker.GetNonCancelMessagesAllowed();

        // Adjust bid side
        if (bidPrices[0] != 0) {
            // Calculate front of the book bid
            unsigned int frontBid = bidPrices[0] + priceAdjustment - ADDITIONAL_SPREAD;
            // Count to compute the maximum bid size
            // and also mark orders for cancellation that are too far away
            long maximumBidSize = POSITION_LIMIT - mPosition;
            for (unsigned int price : mBidPrices) {
                Order* order = mBidToOrder[price];
                //RLOG(LG_AT, LogLevel::LL_INFO) << price << " and volume " << order->volume << " and id " << order->orderId << " with maximumBidSize " << maximumBidSize;
                if (price > frontBid || price <= frontBid - NUM_CLONES * TICK_SIZE_IN_CENTS) {
                    SendCancelOrder(order->orderId);
                    //RLOG(LG_AT, LogLevel::LL_INFO) << "cancelling order " << order->orderId << " at price " << price;
                    //cancelled_orders[index++] = order->orderId;
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
                    //RLOG(LG_AT, LogLevel::LL_INFO) << "putting in order at " << price << " with maximumBidSize " << maximumBidSize << " and id " << orderId;
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
        if (askPrices[0] != 0) {
            // Calculate front of the book ask
            unsigned int frontAsk = askPrices[0] + priceAdjustment - ADDITIONAL_SPREAD;
            // Count to compute the maximum ask size
            // and also mark orders for cancellation that are too far away
            long maximumAskSize = POSITION_LIMIT + mPosition;
            for (unsigned int price : mAskPrices) {
                Order* order = mAskToOrder[price];
                //RLOG(LG_AT, LogLevel::LL_INFO) << price << " and volume " << order->volume << " and id " << order->orderId << " with maximumBidSize " << maximumBidSize;
                if (price < frontAsk || price >= frontAsk + NUM_CLONES * TICK_SIZE_IN_CENTS) {
                    SendCancelOrder(order->orderId);
                    //RLOG(LG_AT, LogLevel::LL_INFO) << "cancelling order " << order->orderId << " at price " << price;
                    //cancelled_orders[index++] = order->orderId;
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
                    //RLOG(LG_AT, LogLevel::LL_INFO) << "putting in order at " << price << " with maximumBidSize " << maximumBidSize << " and id " << orderId;
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

        /*unsigned int sumorders = 0;
        RLOG(LG_AT, LogLevel::LL_INFO) << "Currently active orders are:";
        for (unsigned int price : mBidPrices) {
            RLOG(LG_AT, LogLevel::LL_INFO) << "ID: " << mBidToOrder[price]->orderId << " at price " << price << " for " << mBidToOrder[price]->volume;
            sumorders += mBidToOrder[price]->volume;
        }
        if (mPosition + sumorders > POSITION_LIMIT) {
            RLOG(LG_AT, LogLevel::LL_ERROR) << "mPosition + sumorders = " << (sumorders + mPosition);
        }*/
    }

    RLOG(LG_AT, LogLevel::LL_INFO) << "order book received for " << instrument << " instrument"
                                   << ": ask prices: " << askPrices[0]
                                   << "; ask volumes: " << askVolumes[0]
                                   << "; bid prices: " << bidPrices[0]
                                   << "; bid volumes: " << bidVolumes[0];

    RLOG(LG_AT, LogLevel::LL_INFO) << "making market for ETF " << mBidSize << " @ " << mBidPrice << ":"
                                   << mAskPrice << " @ " << mAskSize;
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
    BaseAutoTrader::SendAmendOrder(clientOrderId, volume);
    NoteMessage();
}

void AutoTrader::SendCancelOrder(unsigned long clientOrderId)
{
    BaseAutoTrader::SendCancelOrder(clientOrderId);
    NoteMessage();
}

void AutoTrader::SendHedgeOrder(unsigned long clientOrderId, Side side, unsigned long price, unsigned long volume)
{
    BaseAutoTrader::SendHedgeOrder(clientOrderId, side, price, volume);
    NoteMessage();
}

void AutoTrader::SendInsertOrder(unsigned long clientOrderId, Side side, unsigned long price, unsigned long volume, Lifespan lifespan)
{
    BaseAutoTrader::SendInsertOrder(clientOrderId, side, price, volume, lifespan);
    NoteMessage();
}

void MessageFrequencyTracker::NoteMessage()
{
    // Add new message and advance pointer
    // For latency reasons, do not remove timed out messages
    ptime currentTime = microsec_clock::universal_time();
    *(mTail++) = currentTime;
    mRollingMessageCount++;
    if (mTail == mMem.end())
        mTail = mMem.begin();
}

int MessageFrequencyTracker::GetNonCancelMessagesAllowed()
{
    // Remove timed out messages
    ptime currentTime = microsec_clock::universal_time();
    while (mHead != mTail && currentTime - *mHead > MessageFrequencyTracker::PeriodLength) {
        mRollingMessageCount--;
        if (++mHead == mMem.end())
            mHead = mMem.begin();
    }
    
    // Figure out what message strategy is guranteed to be compliant
    constexpr unsigned long safetyMargin = 5;
    constexpr unsigned long maxOpenOrders = 2 * NUM_CLONES;
    unsigned long freeMessages = MAX_MESSAGE_FREQ - mRollingMessageCount;
    return std::max(0, (freeMessages - maxOpenOrders - safetyMargin) / 2);
}
// python rtg.py run autotrader
// ./compile.sh
// git pull; ./compile.sh; python rtg.py run autotrader
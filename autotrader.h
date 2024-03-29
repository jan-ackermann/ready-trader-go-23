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
#ifndef CPPREADY_TRADER_GO_AUTOTRADER_H
#define CPPREADY_TRADER_GO_AUTOTRADER_H

#include <array>
#include <memory>
#include <string>
#include <unordered_set>
#include <set>

#include <boost/asio/io_context.hpp>
#include <boost/thread/thread.hpp> 
#include <boost/date_time/posix_time/posix_time.hpp>

#include <ready_trader_go/baseautotrader.h>
#include <ready_trader_go/logging.h>
#include <ready_trader_go/types.h>

struct Order {
    Order(unsigned long price, unsigned long volume, unsigned long orderId) : price(price), volume(volume), orderId(orderId) {}

    unsigned long price, volume, orderId;
};

RTG_INLINE_GLOBAL_LOGGER_WITH_CHANNEL(LG_AT, "AUTO")

constexpr unsigned int LOT_SIZE = 10;
constexpr int POSITION_LIMIT = 100;
constexpr int TICK_SIZE_IN_CENTS = 100;
constexpr int MIN_BID_NEAREST_TICK = (ReadyTraderGo::MINIMUM_BID + TICK_SIZE_IN_CENTS) / TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS;
constexpr int MAX_ASK_NEAREST_TICK = ReadyTraderGo::MAXIMUM_ASK / TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS;

constexpr ReadyTraderGo::Instrument FUT = ReadyTraderGo::Instrument::FUTURE;
constexpr ReadyTraderGo::Instrument ETF = ReadyTraderGo::Instrument::ETF;
constexpr double TAKER_FEE = 0.0002;
constexpr double MAKER_FEE = -0.0001;

constexpr int NUM_CLONES = 5;
constexpr unsigned long ADDITIONAL_SPREAD = 1 * TICK_SIZE_IN_CENTS;
constexpr size_t MAX_MESSAGE_FREQ = 50;

using ptime = boost::posix_time::ptime;
using time_duration = boost::posix_time::time_duration;

class MessageFrequencyTracker {
    using arr_type = std::array<ptime, 16 * MAX_MESSAGE_FREQ>;
    arr_type mMem;
    arr_type::iterator mHead, mTail;
    unsigned long mRollingMessageCount;
    static time_duration PeriodLength;
public:
    MessageFrequencyTracker() : mMem{}, mRollingMessageCount(0) {
        mHead = mMem.begin();
        mTail = mMem.begin();
    }
    void NoteMessage();
    int GetNewOrdersAllowed();
};

class AutoTrader : public ReadyTraderGo::BaseAutoTrader
{
public:
    explicit AutoTrader(boost::asio::io_context& context);

    // Called when the execution connection is lost.
    void DisconnectHandler() override;

    // Called when the matching engine detects an error.
    // If the error pertains to a particular order, then the client_order_id
    // will identify that order, otherwise the client_order_id will be zero.
    void ErrorMessageHandler(unsigned long clientOrderId,
                             const std::string& errorMessage) override;

    // Called when one of your hedge orders is filled, partially or fully.
    //
    // The price is the average price at which the order was (partially) filled,
    // which may be better than the order's limit price. The volume is
    // the number of lots filled at that price.
    //
    // If the order was unsuccessful, both the price and volume will be zero.
    void HedgeFilledMessageHandler(unsigned long clientOrderId,
                                   unsigned long price,
                                   unsigned long volume) override;

    // Called periodically to report the status of an order book.
    // The sequence number can be used to detect missed or out-of-order
    // messages. The five best available ask (i.e. sell) and bid (i.e. buy)
    // prices are reported along with the volume available at each of those
    // price levels.
    void OrderBookMessageHandler(ReadyTraderGo::Instrument instrument,
                                 unsigned long sequenceNumber,
                                 const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT>& askPrices,
                                 const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT>& askVolumes,
                                 const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT>& bidPrices,
                                 const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT>& bidVolumes) override;

    // Called when one of your orders is filled, partially or fully.
    void OrderFilledMessageHandler(unsigned long clientOrderId,
                                   unsigned long price,
                                   unsigned long volume) override;

    // Called when the status of one of your orders changes.
    // The fill volume is the number of lots already traded, remaining volume
    // is the number of lots yet to be traded and fees is the total fees paid
    // or received for this order.
    // Remaining volume will be set to zero if the order is cancelled.
    void OrderStatusMessageHandler(unsigned long clientOrderId,
                                   unsigned long fillVolume,
                                   unsigned long remainingVolume,
                                   signed long fees) override;

    // Called periodically when there is trading activity on the market.
    // The five best ask (i.e. sell) and bid (i.e. buy) prices at which there
    // has been trading activity are reported along with the aggregated volume
    // traded at each of those price levels.
    // If there are less than five prices on a side, then zeros will appear at
    // the end of both the prices and volumes arrays.
    void TradeTicksMessageHandler(ReadyTraderGo::Instrument instrument,
                                  unsigned long sequenceNumber,
                                  const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT>& askPrices,
                                  const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT>& askVolumes,
                                  const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT>& bidPrices,
                                  const std::array<unsigned long, ReadyTraderGo::TOP_LEVEL_COUNT>& bidVolumes) override;

    // Overrides for message frequency tracking
    void SendAmendOrder(unsigned long clientOrderId, unsigned long volume) override;
    void SendCancelOrder(unsigned long clientOrderId) override;
    void SendHedgeOrder(unsigned long clientOrderId, ReadyTraderGo::Side side, unsigned long price, unsigned long volume) override;
    void SendInsertOrder(unsigned long clientOrderId, ReadyTraderGo::Side side, unsigned long price, unsigned long volume, ReadyTraderGo::Lifespan lifespan) override;

private:
    unsigned long mNextMessageId = 1;
    signed long mPosition = 0;
    std::set<unsigned long> mBidPrices;
    std::set<unsigned long> mAskPrices;
    std::unordered_map<unsigned long, Order*> mBidToOrder;
    std::unordered_map<unsigned long, Order*> mBidOrderIdToOrder;
    std::unordered_map<unsigned long, Order*> mAskToOrder;
    std::unordered_map<unsigned long, Order*> mAskOrderIdToOrder;
    MessageFrequencyTracker mMessageTracker;
};

#endif //CPPREADY_TRADER_GO_AUTOTRADER_H

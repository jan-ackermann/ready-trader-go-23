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
#include <boost/date_time/posix_time/posix_time.hpp>

#include <ready_trader_go/baseautotrader.h>
#include <ready_trader_go/types.h>

struct Order {
    Order(unsigned long price, unsigned long volume, unsigned long orderId) : price(price), volume(volume), orderId(orderId) {}

    unsigned long price, volume, orderId;
};

constexpr int MIN_VALID_FUT_ORDER_VOLUME = 100;
constexpr int NUM_CLONES = 3;
constexpr unsigned long ADDITIONAL_SPREAD = 1 * TICK_SIZE_IN_CENTS;
constexpr size_t MAX_MESSAGE_FREQ = 50;

class MessageFrequencyTracker {
    using arr_type = std::array<ptime, 16 * MAX_MESSAGE_FREQ>;
    arr_type mMem;
    arr_type::iterator mHead, mTail;
    unsigned long mRollingMessageCount;
    static constexpr time_duration PeriodLength = seconds(1);
public:
    MessageFrequencyTracker() : mRollingMessageCount(0), mMem{}, mHead(mMem.begin()), mTail(mMem.begin()) {}
    void NoteMessage();
    int GetNonCancelMessagesAllowed();
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
    void SendAmendOrder(unsigned long clientOrderId, unsigned long volume);
    void SendCancelOrder(unsigned long clientOrderId);
    void SendHedgeOrder(unsigned long clientOrderId, Side side, unsigned long price, unsigned long volume);
    void SendInsertOrder(unsigned long clientOrderId, Side side, unsigned long price, unsigned long volume, Lifespan lifespan);

private:
    unsigned long mNextMessageId = 1;
    //unsigned long mBidId = 0;
    unsigned long mBidPrice = 0;
    unsigned long mBidSize = 0;
    unsigned long mAskId = 0;
    unsigned long mAskPrice = 0;
    unsigned long mAskSize = 0;
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

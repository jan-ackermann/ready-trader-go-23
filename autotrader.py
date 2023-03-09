# Copyright 2021 Optiver Asia Pacific Pty. Ltd.
#
# This file is part of Ready Trader Go.
#
#     Ready Trader Go is free software: you can redistribute it and/or
#     modify it under the terms of the GNU Affero General Public License
#     as published by the Free Software Foundation, either version 3 of
#     the License, or (at your option) any later version.
#
#     Ready Trader Go is distributed in the hope that it will be useful,
#     but WITHOUT ANY WARRANTY; without even the implied warranty of
#     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#     GNU Affero General Public License for more details.
#
#     You should have received a copy of the GNU Affero General Public
#     License along with Ready Trader Go.  If not, see
#     <https://www.gnu.org/licenses/>.
import asyncio
import itertools

from typing import List

from ready_trader_go import BaseAutoTrader, Instrument, Lifespan, MAXIMUM_ASK, MINIMUM_BID, Side


LOT_SIZE = 10
POSITION_LIMIT = 100
TICK_SIZE_IN_CENTS = 100
MIN_BID_NEAREST_TICK = (MINIMUM_BID + TICK_SIZE_IN_CENTS) // TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS
MAX_ASK_NEAREST_TICK = MAXIMUM_ASK // TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS

FUT = Instrument.FUTURE
ETF = Instrument.ETF
TAKER_FEE = 0.0002
MAKER_FEE = -0.0001


class AutoTrader(BaseAutoTrader):
    """Example Auto-trader.

    When it starts this auto-trader places ten-lot bid and ask orders at the
    current best-bid and best-ask prices respectively. Thereafter, if it has
    a long position (it has bought more lots than it has sold) it reduces its
    bid and ask prices. Conversely, if it has a short position (it has sold
    more lots than it has bought) then it increases its bid and ask prices.
    """

    def __init__(self, loop: asyncio.AbstractEventLoop, team_name: str, secret: str):
        """Initialise a new instance of the AutoTrader class."""
        super().__init__(loop, team_name, secret)
        self.order_ids = itertools.count(1)
        self.bids = set()
        self.asks = set()
        self.arb_bids = set()
        self.arb_asks = set()
        self.ask_id = self.ask_price = self.bid_id = self.bid_price = self.position = 0
        self.future_mid_price = 0
        self.order_books = dict()

    def on_error_message(self, client_order_id: int, error_message: bytes) -> None:
        """Called when the exchange detects an error.

        If the error pertains to a particular order, then the client_order_id
        will identify that order, otherwise the client_order_id will be zero.
        """
        self.logger.warning("error with order %d: %s", client_order_id, error_message.decode())
        if client_order_id != 0 and (client_order_id in self.bids or client_order_id in self.asks):
            self.on_order_status_message(client_order_id, 0, 0, 0)

    def on_hedge_filled_message(self, client_order_id: int, price: int, volume: int) -> None:
        """Called when one of your hedge orders is filled.

        The price is the average price at which the order was (partially) filled,
        which may be better than the order's limit price. The volume is
        the number of lots filled at that price.
        """
        self.logger.info("received hedge filled for order %d with average price %d and volume %d", client_order_id,
                         price, volume)

    def find_arbitrage(self, sequence_number: int):
        if FUT not in self.order_books or self.order_books[FUT]['seq'] != sequence_number:
            return
        if ETF not in self.order_books or self.order_books[ETF]['seq'] != sequence_number:
            return

        # Up-to-date order books
        # Will trade only the first of (potentially) multiple crossed orders

        # Detect ETF > FUTURE cross
        ETF_bids = self.order_books[ETF]['bid_prices']
        FUT_asks = self.order_books[FUT]['ask_prices']
        diff = ETF_bids[0] - FUT_asks[0]
        if diff > 0 and diff > (ETF_bids[0] * TAKER_FEE):
            if ETF_bids[0] not in self.bids:
                order_id = next(self.order_ids)
                size = min(self.order_books[ETF]['bid_volumes'][0], self.order_books[FUT]['ask_volumes'][0])
                size = max(0, min(POSITION_LIMIT - LOT_SIZE + self.position, size))
                self.logger.info(f"Trading ETF>FUT crossed market by selling {size}@{ETF_bids[0]}, expecting {diff} with {ETF_bids[0] * TAKER_FEE} in fees")
                self.send_insert_order(order_id, Side.SELL, ETF_bids[0], size, Lifespan.FILL_AND_KILL)
                self.arb_asks.add(order_id)

        # Detect FUTURE > ETF cross
        ETF_asks = self.order_books[ETF]['ask_prices']
        FUT_bids = self.order_books[FUT]['bid_prices']
        diff = FUT_bids[0] - ETF_asks[0]
        if diff > 0 and diff > (ETF_asks[0] * TAKER_FEE):
            if ETF_asks[0] not in self.asks:
                order_id = next(self.order_ids)
                size = min(self.order_books[ETF]['ask_volumes'][0], self.order_books[FUT]['bid_volumes'][0])
                size = max(0, min(POSITION_LIMIT - LOT_SIZE - self.position, size))
                self.logger.info(f"Trading FUT>ETF crossed market by buying {size}@{ETF_asks[0]}, expecting {diff} with {ETF_asks[0] * TAKER_FEE} in fees")
                self.send_insert_order(order_id, Side.BUY, ETF_asks[0], size, Lifespan.FILL_AND_KILL)
                self.arb_bids.add(order_id)

    def on_order_book_update_message(self, instrument: int, sequence_number: int, ask_prices: List[int],
                                     ask_volumes: List[int], bid_prices: List[int], bid_volumes: List[int]) -> None:
        """Called periodically to report the status of an order book.

        The sequence number can be used to detect missed or out-of-order
        messages. The five best available ask (i.e. sell) and bid (i.e. buy)
        prices are reported along with the volume available at each of those
        price levels.
        """
        self.logger.info("received order book for instrument %d with sequence number %d", instrument,
                         sequence_number)

        self.order_books[instrument] = {'seq': sequence_number, 'ask_prices': ask_prices, 'ask_volumes': ask_volumes,
                                        'bid_prices': bid_prices, 'bid_volumes': bid_volumes}
        self.find_arbitrage(sequence_number)

        if instrument == Instrument.FUTURE:
            price_adjustment = - (self.position // (3 * LOT_SIZE)) * TICK_SIZE_IN_CENTS

            # for index, price in enumerate(bid_prices):
            #     if bid_volumes[index] >= 20:
            #         best_valid_bid = price
            #         break
            # else:
            #     best_valid_bid = filter(lambda x: x != 0, bid_prices)[-1]

            # for index, price in enumerate(ask_prices):
            #     if ask_volumes[index] >= 20:
            #         best_valid_ask = price
            #         break
            # else:
            #     best_valid_ask = filter(lambda x: x != 0, ask_prices)[-1]

            self.future_mid_price = int(round((bid_prices[0] + ask_prices[0]) / 2, -2))
            self.logger.info(f"Future trading at {bid_prices[0]}:{ask_prices[0]} with mid price {self.future_mid_price}")

            additional_spread = 1 * TICK_SIZE_IN_CENTS

            new_bid_price = bid_prices[0] + price_adjustment - additional_spread if bid_prices[0] != 0 else 0
            new_ask_price = ask_prices[0] + price_adjustment + additional_spread if ask_prices[0] != 0 else 0

            if self.bid_id != 0 and new_bid_price not in (self.bid_price, 0):
                self.send_cancel_order(self.bid_id)
                self.bid_id = 0
            if self.ask_id != 0 and new_ask_price not in (self.ask_price, 0):
                self.send_cancel_order(self.ask_id)
                self.ask_id = 0

            if self.bid_id == 0 and new_bid_price != 0 and self.position < POSITION_LIMIT:
                self.bid_id = next(self.order_ids)
                self.bid_price = new_bid_price
                self.send_insert_order(self.bid_id, Side.BUY, new_bid_price, LOT_SIZE, Lifespan.GOOD_FOR_DAY)
                self.bids.add(self.bid_id)

            if self.ask_id == 0 and new_ask_price != 0 and self.position > -POSITION_LIMIT:
                self.ask_id = next(self.order_ids)
                self.ask_price = new_ask_price
                self.send_insert_order(self.ask_id, Side.SELL, new_ask_price, LOT_SIZE, Lifespan.GOOD_FOR_DAY)
                self.asks.add(self.ask_id)

    def on_order_filled_message(self, client_order_id: int, price: int, volume: int) -> None:
        """Called when one of your orders is filled, partially or fully.

        The price is the price at which the order was (partially) filled,
        which may be better than the order's limit price. The volume is
        the number of lots filled at that price.
        """
        self.logger.info("received order filled for order %d with price %d and volume %d", client_order_id,
                         price, volume)

        buy_set = self.bids | self.arb_bids
        sell_set = self.asks | self.arb_asks

        if self.future_mid_price != 0 and client_order_id > 10:
            self.logger.info(f"hedging with future at mid price of {self.future_mid_price}")
            if client_order_id in buy_set:
                self.position += volume
                self.send_hedge_order(next(self.order_ids), Side.ASK, self.future_mid_price, volume)
            elif client_order_id in sell_set:
                self.position -= volume
                self.send_hedge_order(next(self.order_ids), Side.BID, self.future_mid_price, volume)
        else:
            if client_order_id in buy_set:
                self.position += volume
                self.send_hedge_order(next(self.order_ids), Side.ASK, MIN_BID_NEAREST_TICK, volume)
            elif client_order_id in sell_set:
                self.position -= volume
                self.send_hedge_order(next(self.order_ids), Side.BID, MAX_ASK_NEAREST_TICK, volume)

    def on_order_status_message(self, client_order_id: int, fill_volume: int, remaining_volume: int,
                                fees: int) -> None:
        """Called when the status of one of your orders changes.

        The fill_volume is the number of lots already traded, remaining_volume
        is the number of lots yet to be traded and fees is the total fees for
        this order. Remember that you pay fees for being a market taker, but
        you receive fees for being a market maker, so fees can be negative.

        If an order is cancelled its remaining volume will be zero.
        """
        self.logger.info("received order status for order %d with fill volume %d remaining %d and fees %d",
                         client_order_id, fill_volume, remaining_volume, fees)
        if remaining_volume == 0:
            if client_order_id == self.bid_id:
                self.bid_id = 0
            elif client_order_id == self.ask_id:
                self.ask_id = 0

            # It could be either a bid or an ask
            self.bids.discard(client_order_id)
            self.asks.discard(client_order_id)

            self.arb_bids.discard(client_order_id)
            self.arb_asks.discard(client_order_id)

    def on_trade_ticks_message(self, instrument: int, sequence_number: int, ask_prices: List[int],
                               ask_volumes: List[int], bid_prices: List[int], bid_volumes: List[int]) -> None:
        """Called periodically when there is trading activity on the market.

        The five best ask (i.e. sell) and bid (i.e. buy) prices at which there
        has been trading activity are reported along with the aggregated volume
        traded at each of those price levels.

        If there are less than five prices on a side, then zeros will appear at
        the end of both the prices and volumes arrays.
        """
        self.logger.info("received trade ticks for instrument %d with sequence number %d", instrument,
                         sequence_number)

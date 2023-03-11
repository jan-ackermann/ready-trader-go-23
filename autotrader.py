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
from math import floor

from typing import List, Dict

from ready_trader_go import BaseAutoTrader, Instrument, Lifespan, MAXIMUM_ASK, MINIMUM_BID, Side

import numpy as np


LOT_SIZE = 10
POSITION_LIMIT = 100
TICK_SIZE_IN_CENTS = 100
MIN_BID_NEAREST_TICK = (MINIMUM_BID + TICK_SIZE_IN_CENTS) // TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS
MAX_ASK_NEAREST_TICK = MAXIMUM_ASK // TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS

FUT = Instrument.FUTURE
ETF = Instrument.ETF
TAKER_FEE = 0.0002
MAKER_FEE = -0.0001

SUBTRADERS = ['MM', 'ARB']


class Order:
    def __init__(self, order_id: int, instrument: Instrument, price: int, volume: int, side: Side, subtrader: str):
        self.id = order_id
        self.instrument = instrument
        self.price = price
        self.volume = volume
        self.side = side
        self.subtrader = subtrader


class DataStore:
    def __init__(self, auto_trader):
        self.auto_trader = auto_trader
        self.sequence_numbers = {'order_book': 0, 'trade_ticks': 0}

        # Position array (Instrument x Subtrader)
        self.positions = np.ndarray(shape=(2, 2), dtype=int)
        # Active orders
        self.active_orders: Dict[int, Order] = dict()
        self.order_books: Dict[Instrument, List[np.ndarray]] = dict()

    def get_position(self, instrument: Instrument, subtrader: str = 'ALL'):
        if subtrader == 'ALL':
            return self.positions[instrument, :].sum()

        try:
            subtrader_index = SUBTRADERS.index(subtrader)
            return self.positions[instrument, subtrader_index]
        except ValueError as e:
            self.auto_trader.logger.warning(f"Tried to retrieve position from DataStore for"
                                            f"invalid subtrader {subtrader}!")
            return 0

    def get_order_book(self, instrument: Instrument, include_own: bool):
        book = self.order_books[instrument].copy()
        if include_own:
            return book
        else:
            # Very strict for now. Does not subtract own order volume from order book, but removes orders at the same
            # price as ones we have placed for this instrument
            prices = list()
            for order_id, order in self.active_orders.items():
                prices.append(order.price)
            prices = np.array(prices)

            bid_prices, bid_volumes, ask_prices, ask_volumes = book
            bid_mask = (bid_prices != prices).all()
            ask_mask = (ask_prices != prices).all()
            return [bid_prices[bid_mask], bid_volumes[bid_mask], ask_prices[ask_mask], ask_volumes[ask_mask]]

    def integrate_new_order_book(self, instrument: Instrument, order_book):
        seq = self.sequence_numbers['order_book']
        if order_book['seq'] < seq:
            # Discard order book, because an order book for an instrument with a higher sequence number was previously
            # received
            return f"Received out-of-order order book update message with sequence number {order_book['seq']} for" \
                   f"instrument {instrument}, but expected a sequence number of {seq} or above!"

        # Accept order book as in sequence or ahead of sequence (fast-forward then)
        self.sequence_numbers['order_book'] = seq

        # Trim invalid orders
        bid_prices, bid_volumes = np.array(order_book['bid_prices']), np.array(order_book['bid_volumes'])
        ask_prices, ask_volumes = np.array(order_book['ask_prices']), np.array(order_book['ask_volumes'])
        mask = bid_prices != 0
        bid_prices, bid_volumes = bid_prices[mask], bid_volumes[mask]
        mask = ask_prices != 0
        ask_prices, ask_volumes = ask_prices[mask], ask_volumes[mask]
        self.order_books[instrument] = [bid_prices.copy(), bid_volumes.copy(), ask_prices.copy(), ask_volumes.copy()]


class ComplianceLayer:
    def __init__(self, auto_trader):
        self.auto_trader = auto_trader
        self.data: DataStore = self.auto_trader.data_store
        self.position_allowances = dict()
        self.order_ids = itertools.count(1)

    def init_allowances(self):
        ratio_allowances = dict()
        ratio_allowances['MM'] = 0.4
        ratio_allowances['ARB'] = 0.6

        # Turn these ratios into (integer) lots
        self.position_allowances = {subtrader_name: int(floor(ratio * POSITION_LIMIT)) for subtrader_name, ratio in ratio_allowances}

    def is_state_legal(self) -> bool:
        # Check current overall position within bounds
        if not(-POSITION_LIMIT <= self.data.get_position(ETF) <= POSITION_LIMIT):
            return False
        if not (-POSITION_LIMIT <= self.data.get_position(FUT) <= POSITION_LIMIT):
            return False

        # Check current position within bounds for subtraders
        for subtrader_name in SUBTRADERS:
            allowance = self.position_allowances[subtrader_name]
            if not(-allowance <= self.data.get_position(ETF, subtrader_name) <= allowance):
                return False
            if not(-allowance <= self.data.get_position(FUT, subtrader_name) <= allowance):
                return False

        # Check that no combination of trade executions can exceed the overall position limits
        # ...

        # Check that no combination of trade executions can exceed the subtrader position allowance
        # ...

        return True

    def cancel_order(self, order_id: int):
        # There is no compliance logic to perform here, cancelled orders do not bring risk
        if order_id in self.data.active_orders:
            del self.data.active_orders[order_id]
            # TODO Maybe also notify the subtrader
        else:
            self.auto_trader.logger.warning(f"Tried to cancel order with id {order_id}, that does not (anymore) have "
                                            f"an internal record!")
        self.auto_trader.send_cancel_order(order_id)

    def place_order(self, instrument: Instrument, side: Side, price: int,
                    volume: int, lifespan: Lifespan, subtrader: str) -> int:
        if instrument not in (ETF, FUT):
            self.auto_trader.logger.warning(f"Attempted to trade in unknown instrument {instrument}!")
            return -1

        order_id = next(self.order_ids)

        # Try place order internally
        self.data.active_orders[order_id] = Order(order_id, instrument, price, volume, side, subtrader)

        # Test legality
        if self.is_state_legal():
            if instrument == ETF:
                self.auto_trader.send_insert_order(order_id, side, price, volume, lifespan)
            elif instrument == FUT:
                self.auto_trader.send_hedge_order(order_id, side, price, volume)
            return order_id
        else:
            # Reject order
            del self.data.active_orders[order_id]
            self.auto_trader.logger.warning(f"Tried to place trade that would have resulted in an illegal state in "
                                            f"instrument {instrument} by subtrader {subtrader}")
            return -1

    def panic(self):
        # Try to recover
        # Goals
        # 1. Within position limits even if worst case order fill in next tick
        # 2. Delta neutral
        # 3. Reset subtraders sensibly and functionally
        # 4. Avoid cycling violations, e.g. random exponential restart...
        # 5. Minimize downtime
        pass


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
        self.bids = set()
        self.asks = set()
        self.arb_bids = set()
        self.arb_asks = set()
        self.ask_id = self.ask_price = self.bid_id = self.bid_price = self.position = 0
        self.future_mid_price = 0
        self.order_books = dict()

        self.orders_by_instrument = dict()

        self.data_store = DataStore(self)
        self.compliance_layer = ComplianceLayer(self)

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
                max_allowed = POSITION_LIMIT - LOT_SIZE + self.position
                position_target = int(round(((POSITION_LIMIT - LOT_SIZE) / 3.0) * diff))
                size = max(0, min(max_allowed, size, position_target + self.position))
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
                max_allowed = POSITION_LIMIT - LOT_SIZE - self.position
                position_target = int(round(((POSITION_LIMIT - LOT_SIZE) / 3.0) * diff))
                size = max(0, min(max_allowed, size, position_target - self.position))
                self.logger.info(f"Trading FUT>ETF crossed market by buying {size}@{ETF_asks[0]}, expecting {diff} with {ETF_asks[0] * TAKER_FEE} in fees")
                self.send_insert_order(order_id, Side.BUY, ETF_asks[0], size, Lifespan.FILL_AND_KILL)
                self.arb_bids.add(order_id)

    def on_order_book_update_message(self, instrument: Instrument, sequence_number: int, ask_prices: List[int],
                                     ask_volumes: List[int], bid_prices: List[int], bid_volumes: List[int]) -> None:
        """Called periodically to report the status of an order book.

        The sequence number can be used to detect missed or out-of-order
        messages. The five best available ask (i.e. sell) and bid (i.e. buy)
        prices are reported along with the volume available at each of those
        price levels.
        """
        self.logger.info("received order book for instrument %d with sequence number %d", instrument,
                         sequence_number)

        book = {'seq': sequence_number, 'bid_prices': bid_prices, 'bid_volumes': bid_volumes,
                                        'ask_prices': ask_prices, 'ask_volumes': ask_volumes}
        self.order_books[instrument] = book

        err_msg = self.data_store.integrate_new_order_book(instrument, book)
        if err_msg is not None:
            self.logger.warn(err_msg)


        #self.find_arbitrage(sequence_number)

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
                bid_size = max(0, min(LOT_SIZE, POSITION_LIMIT - self.position))
                self.send_insert_order(self.bid_id, Side.BUY, new_bid_price, bid_size, Lifespan.GOOD_FOR_DAY)
                self.bids.add(self.bid_id)

            if self.ask_id == 0 and new_ask_price != 0 and self.position > -POSITION_LIMIT:
                self.ask_id = next(self.order_ids)
                self.ask_price = new_ask_price
                ask_size = max(0, min(LOT_SIZE, POSITION_LIMIT + self.position))
                self.send_insert_order(self.ask_id, Side.SELL, new_ask_price, ask_size, Lifespan.GOOD_FOR_DAY)
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

        if self.future_mid_price != 0 and False:  # client_order_id > 10:
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

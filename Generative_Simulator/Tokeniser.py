import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
from pathlib import Path
from scipy.stats import binned_statistic 

def quantile_binning(file_path, feature_name, bin_num):
    msgs = pd.read_csv(file_path, names=["time","type","order_id","size","price","direction"])
    msgs = msgs[msgs["type"].isin({1,2,3})].reset_index(drop = True)
    if feature_name == "time":
        features = msgs[feature_name].diff().dropna()
    else: 
        features = msgs[feature_name]
    quantile_bins = np.unique(np.quantile(features, np.linspace(0,1,bin_num+1)))
    centers, _, _ = binned_statistic(features, features, statistic='median', bins=quantile_bins)

    return quantile_bins, centers 




class Tokeniser: 
    # Token IDs
    PAD, BOS, EOS = 0, 1, 2                                                                                                                  
    ACTION_BASE = 3
    PRICE_BASE  = 9                                                                                                                          
    SIZE_BASE   = 70                                                                                                                       
    DT_BASE     = 90
    EMPTY = 130                                                                                                                         
    VOCAB_SIZE  = 131
    


    action_type = np.array([[0,0],[3,4],[7,8],[5,6]])   
    """
    ⏺                 ┌─────────────────────┬─────────────────────┐                                                                                                                                 
                      │   dir_idx = 0       │   dir_idx = 1       │                                                                                                                                 
                      │   (direction = +1,  │   (direction = -1,  │                                                                                                                                 
                      │    BUY)             │    SELL)            │
  ┌───────────────────┼─────────────────────┼─────────────────────┤                                                                                                                                 
  │ order_type = 0    │         0           │         0           │                                                                                                                                 
  │ (unused padding)  │                     │                     │                                                                                                                                 
  ├───────────────────┼─────────────────────┼─────────────────────┤                                                                                                                                 
  │ order_type = 1    │         3           │         4           │
  │ (NEW)             │     NEW_BUY         │     NEW_SELL        │                                                                                                                                 
  ├───────────────────┼─────────────────────┼─────────────────────┤                                                                                                                                 
  │ order_type = 2    │         7           │         8           │
  │ (MODIFY)          │     MODIFY_BUY      │     MODIFY_SELL     │                                                                                                                                 
  ├───────────────────┼─────────────────────┼─────────────────────┤
  │ order_type = 3    │         5           │         6           │                                                                                                                                 
  │ (CANCEL)          │     CANCEL_BUY      │     CANCEL_SELL     │
  └───────────────────┴─────────────────────┴─────────────────────┘      
    """

    # vectors to input 
    action_type_reverse = np.array([[0,0], [0,0], [0,0], [1,1], [1,-1], [3,1], [3,-1], [2,1], [2,-1]])

    # PRICE
    # LOBSTER stores prices as dollars × 10,000 (per the readme: $91.14 → 911400). The real-world tick for US equities  
    # is $0.01, so in LOBSTER's integer units that's 0.01 × 10000 = 100. 
    TICK = 100 
    PRICE_LINEAR_RANGE   = 20   # linear range for the price tokenisation 
    PRICE_LOG_RANGE = 10
    PRICE_LOG_FAR_EDGE  = np.array([21, 30, 45, 70, 100, 150, 220, 320, 480, 700])   # log bucketed range

    # VOLUME(SIZE)
    VOLUME_LOG = np.geomspace(1,150000,21).astype(int)

    def __init__(self, message_file):
        #DT(delta T)
        self.TIME_QUANTILE_EDGE, self.TIME_MEDIAN = quantile_binning(message_file, "time", 40)


    """
      bucket:   0  …  9 │ 10  …  29 │ 30  │ 31  …  50 │ 51  …  60                                                                                                                                       
            agg far │  agg lin  │ best│  pass lin │  pass far                                                                                                                                       
            (off<0) │  (off<0)  │ (=0)│  (off>0)  │  (off>0)  
    """

    def price_to_token(self, price: pd.DataFrame, best_same_side: pd.DataFrame, direction: pd.DataFrame):
        offset = ((best_same_side - price) // self.TICK) * direction
        mag = abs(offset)
        log_bucket = np.clip(np.searchsorted(self.PRICE_LOG_FAR_EDGE, mag, side = 'right') - 1, 0, self.PRICE_LOG_RANGE - 1)
        
        tokens = np.select(
            [mag == 0, mag <= self.PRICE_LINEAR_RANGE, (self.PRICE_LINEAR_RANGE < mag) & (offset < 0), (self.PRICE_LINEAR_RANGE < mag) & (offset > 0)],
            [self.PRICE_BASE + 30, self.PRICE_BASE + offset + 30, self.PRICE_BASE + 9 - log_bucket, self.PRICE_BASE + 51 + log_bucket]
        )
        # book.shift(1) introduces NaN at row 0, which promotes book columns to
        # float64. np.select then returns float; tokens must be int.
        return tokens.astype(int)
    
    def token_to_price(self, token, best_same_side, direction):                                                                                                                                       
        bucket = np.asarray(token) - self.PRICE_BASE
                                                                                                                                                                                                        
        # Geometric-mean center of each log bucket. Append a synthetic top edge so                                                                                                                  
        # the last bucket has both endpoints (mirrors the scalar `EDGE[-1] * 2`).                                                                                                                     
        edges = np.append(self.PRICE_LOG_FAR_EDGE, self.PRICE_LOG_FAR_EDGE[-1] * 2)                                                                                                                   
                                                                                                                                                                                                        
        # Log-bucket index for each far region. Clip so indexing is always safe;                                                                                                                      
        # wrong-branch values are discarded by np.select.                                                                                                                                             
        agg_far_idx  = np.clip(9 - bucket,  0, self.PRICE_LOG_RANGE - 1)                                                                                                                            
        pass_far_idx = np.clip(bucket - 51, 0, self.PRICE_LOG_RANGE - 1)
                                                                                                                                                                                                        
        agg_far_mag  = np.sqrt(edges[agg_far_idx]  * edges[agg_far_idx  + 1]).astype(int)                                                                                                             
        pass_far_mag = np.sqrt(edges[pass_far_idx] * edges[pass_far_idx + 1]).astype(int)                                                                                                             
                                                                                                                                                                                                        
        offset = np.select(                                                                                                                                                                         
            [(bucket >= 10) & (bucket <= 50), bucket <= 9, bucket >= 51],                                                                                                                             
            [bucket - 30,                    -agg_far_mag, pass_far_mag],                                                                                                                          
        )                                                                                                                                                                                             
    
        return best_same_side - direction * offset * self.TICK 


    def size_to_token(self, size):
        log_bucket = np.searchsorted(self.VOLUME_LOG, size, side = "right") - 1 
        log_bucket = np.clip(log_bucket, 0, 19)
        return self.SIZE_BASE + log_bucket
    
    def token_to_size(self, bucket_number):
        bucket = np.asarray(bucket_number) - self.SIZE_BASE
        l = self.VOLUME_LOG[bucket]
        r = self.VOLUME_LOG[bucket + 1]
        return np.sqrt(l * r).astype(int)
    
    def time_to_token(self, time):
        quantile_bucket = np.searchsorted(self.TIME_QUANTILE_EDGE, time, side = "right") - 1
        quantile_bucket = np.clip(quantile_bucket, 0, 39)
        bucket = quantile_bucket + self.DT_BASE
        return bucket 
    
    def token_to_time(self, bucket_number):
        bucket = bucket_number - self.DT_BASE
        return self.TIME_MEDIAN[bucket]
    
    def action_to_token(self, order_type, direction): 
    
        return self.action_type[order_type, np.where(direction==1, 0, 1)]
    
    def token_to_action(self, token): 
        pair = self.action_type_reverse[token]
        if pair.shape == (2,):
            return pair[0], pair[1]
        else:
            return pair[:, 0], pair[:, 1] 


    # the actual encode function we call
    def encode_training(self, message: pd.DataFrame, book: pd.DataFrame) -> np.ndarray:
        # 1. Shift book so row i = state BEFORE message i (for every original row).
        book = book.shift(1)

        # 2. Filter by message type (book must follow the same mask).
        mask    = message["type"].isin([1, 2, 3])
        book    = book[mask].reset_index(drop=True)
        message = message[mask].reset_index(drop=True)

        # 3. Compute dt on the filtered sequence (matches quantile_binning's source).
        dt = message["time"].diff()      # row 0 NaN — no kept predecessor

        # 4. Drop row 0 — its book is NaN (no pre-state for the first ever message)
        #    AND its dt is NaN (no previous kept message).
        book    = book.iloc[1:].reset_index(drop=True)
        message = message.iloc[1:].reset_index(drop=True)
        dt      = dt.iloc[1:].reset_index(drop=True)

        price      = message["price"]
        direction  = message["direction"]
        size       = message["size"]
        order_type = message["type"]

        size_tokens   = self.size_to_token(size)
        time_tokens   = self.time_to_token(dt)
        action_tokens = self.action_to_token(order_type, direction)

        best_ask = book["ask_p1"].copy()
        best_bid = book["bid_p1"].copy()
        best_same_side = np.where(direction == 1, best_bid, best_ask)
        price_tokens   = self.price_to_token(price, best_same_side, direction)


        price_cols = []
        volume_cols = []
        for lvl in range(1,11):
            price_cols += [f"ask_p{lvl}", f"bid_p{lvl}"]
            volume_cols += [f"ask_s{lvl}", f"bid_s{lvl}"]


        for i in range(len(price_cols)):
            if i % 2 == 0:
                book[price_cols[i]] = np.where(book[volume_cols[i]] == 0, self.EMPTY, self.price_to_token(book[price_cols[i]], best_ask, -1))
                book[volume_cols[i]] = np.where(book[volume_cols[i]] == 0, self.EMPTY, self.size_to_token(book[volume_cols[i]]))
            else:
                book[price_cols[i]] = np.where(book[volume_cols[i]] == 0, self.EMPTY, self.price_to_token(book[price_cols[i]], best_bid, 1))
                book[volume_cols[i]] = np.where(book[volume_cols[i]] == 0, self.EMPTY, self.size_to_token(book[volume_cols[i]]))

        book_tokens = book.to_numpy(dtype = np.int16)

        return np.stack([action_tokens, size_tokens, price_tokens, time_tokens], axis = 1).reshape(-1), book_tokens 
    




        

        





def main():
    ROOT = Path(__file__).resolve().parent.parent
    message_file  = ROOT / "LOBSTER_SampleFile_AAPL_2012-06-21_10" \
            / "AAPL_2012-06-21_34200000_57600000_message_10.csv"
    book_file = ROOT / "LOBSTER_SampleFile_AAPL_2012-06-21_10" \
            / "AAPL_2012-06-21_34200000_57600000_orderbook_10.csv"
    msgs = pd.read_csv(
        message_file, 
        names= ["time", "type", "order_id", "size", "price", "direction"]
    )
    orderbook_cols = []
    for lvl in range(1,11):
        orderbook_cols += [f"ask_p{lvl}", f"ask_s{lvl}", f"bid_p{lvl}", f"bid_s{lvl}"]

    book = pd.read_csv(
        book_file,
        header = None,
        names = orderbook_cols,
    )
    tokeniser = Tokeniser(message_file)
    tokenised_message_dataset, tokenised_book_tokens = tokeniser.encode_training(msgs, book)
    output_path_1 = ROOT / "Generative_Simulator" /"Data" / "message_tokens.npz"
    output_path_2 = ROOT / "Generative_Simulator" /"Data" / "book_tokens.npz"
    np.savez(output_path_1, tokenised_message_dataset.astype(np.int16))
    np.savez(output_path_2, tokenised_book_tokens.astype(np.int16))
    print(f"Saved {len(tokenised_message_dataset)} tokens to {output_path_1} and {len(tokenised_book_tokens)} tokens to {output_path_2}"                                                                                                                                     
          f"({output_path_1.stat().st_size / 1024:.1f} KB)"
          f"({output_path_2.stat().st_size / 1024:.1f} KB)")
    


if __name__ == "__main__":
    main()
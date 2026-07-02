import torch
from torch.utils.data import DataLoader, Dataset
import numpy as np
from pathlib import Path


class LOBDataset(Dataset):
    def __init__(self, tokenised_message_dataset_path: Path, tokenised_book_dataset_path: Path,  window_size: int = 512):
        assert window_size % 4 == 0
        self.message_tokens :np.ndarray = np.load(tokenised_message_dataset_path)["arr_0"]
        self.book_tokens :np.ndarray = np.load(tokenised_book_dataset_path)["arr_0"]
        self.window : int = window_size       # one datapoint has four message_tokens
        self.n_start : int= (len(self.message_tokens) - self.window) // 4

    def __len__(self):
        return self.n_start

    def __getitem__(self,idx :int):
        book_state = self.book_tokens[idx: idx + self.window//4]
        idx = idx * 4
        input = self.message_tokens[idx: idx + self.window]
        label = self.message_tokens[idx + 1: idx + self.window + 1]
        return torch.from_numpy(input).long(), torch.from_numpy(label).long(), torch.from_numpy(book_state).long()


import torch 
import numpy as np
from torch.utils.data import DataLoader, Dataset
import torch.nn as nn
import torch.nn.functional as F

"""
REFERENCE (GPT style autoregressive transformer): 
import torch
import torch.nn as nn
import torch.nn.functional as F

class CausalSelfAttention(nn.Module):
    def __init__(self, d_model, n_heads):
        super().__init__()
        self.n_heads = n_heads
        self.qkv = nn.Linear(d_model, 3 * d_model)
        self.proj = nn.Linear(d_model, d_model)

    def forward(self, x):
        B, T, C = x.shape
        q, k, v = self.qkv(x).chunk(3, dim=-1)
        # reshape to (B, n_heads, T, head_dim)
        q, k, v = [t.view(B, T, self.n_heads, C // self.n_heads).transpose(1, 2)
                   for t in (q, k, v)]
        # is_causal=True applies the lower-triangular mask: position t sees only <= t
        out = F.scaled_dot_product_attention(q, k, v, is_causal=True)
        out = out.transpose(1, 2).reshape(B, T, C)
        return self.proj(out)

class Block(nn.Module):
    def __init__(self, d_model, n_heads):
        super().__init__()
        self.ln1 = nn.LayerNorm(d_model)
        self.attn = CausalSelfAttention(d_model, n_heads)
        self.ln2 = nn.LayerNorm(d_model)
        self.mlp = nn.Sequential(
            nn.Linear(d_model, 4 * d_model), nn.GELU(),
            nn.Linear(4 * d_model, d_model),
        )

    def forward(self, x):
        x = x + self.attn(self.ln1(x))   # residual connections
        x = x + self.mlp(self.ln2(x))
        return x

class GPT(nn.Module):
    def __init__(self, vocab_size, d_model=256, n_heads=4, n_layers=4, max_len=128):
        super().__init__()
        self.tok_emb = nn.Embedding(vocab_size, d_model)
        self.pos_emb = nn.Embedding(max_len, d_model)
        self.blocks = nn.ModuleList([Block(d_model, n_heads) for _ in range(n_layers)])
        self.ln_f = nn.LayerNorm(d_model)
        self.head = nn.Linear(d_model, vocab_size, bias=False)

    def forward(self, idx, targets=None):
        B, T = idx.shape
        pos = torch.arange(T, device=idx.device)
        x = self.tok_emb(idx) + self.pos_emb(pos)
        for block in self.blocks:
            x = block(x)
        logits = self.head(self.ln_f(x))   # (B, T, vocab_size)

        loss = None
        if targets is not None:
            # predict token t+1 from tokens <= t  → the autoregressive objective
            loss = F.cross_entropy(logits.view(-1, logits.size(-1)), targets.view(-1))
        return logits, loss
"""

class CausalSelfAttention(nn.Module):
    def __init__(self, d_model :int, num_heads :int):
        super().__init__()
        self.d_model = d_model
        self.num_heads = num_heads
        self.qkv = nn.Linear(d_model, 3 * d_model)  # Q K V transformation
        self.out_proj = nn.Linear(d_model, d_model)  # output projection to mix the multi-headed results

    def forward(self, x):  # x represents a batch of sequences, (B, seq_len, D)
        batch, seq_len, d_model = x.shape  # (B, seq_len, D)
        q, k, v = self.qkv(x).chunk(chunks = 3, dim = -1)   # q, k, v -> (B, seq_len, D)
        q, k, v = [i.view(batch, seq_len, self.num_heads, d_model//self.num_heads).transpose(1,2) for i in (q,k,v)] # q, k, v -> (B, num_heads, seq_len, d)
        out = F.scaled_dot_product_attention(q, k, v, is_causal =True)   # out -> (B, num_heads, seq_len, d)
        out = out.transpose(1,2).contiguous().view(batch, seq_len, d_model)  # out -> (B, seq_len, D)
        out = self.out_proj(out)
        return out

class Block(nn.Module): 
    def __init__(self, d_model: int, num_heads :int):
        super().__init__()
        self.ln_1 = nn.LayerNorm(d_model)
        self.attention = CausalSelfAttention(d_model, num_heads)
        self.ln_2 = nn.LayerNorm(d_model)
        self.ffn_1 = nn.Linear(d_model, 4 * d_model)
        self.gelu = nn.GELU()
        self.ffn_2 = nn.Linear(4 * d_model, d_model)

    def forward(self, x):
        x = x + self.attention(self.ln_1(x))
        x = x + self.ffn_2(self.gelu(self.ffn_1(self.ln_2(x))))
        return x

class Simulator(nn.Module):
    def __init__(self, vocab_size :int, num_heads :int, d_model :int, num_layers :int, max_seq_len: int):
        super().__init__()
        self.position_embedding = nn.Embedding(max_seq_len, d_model)
        self.token_embedding = nn.Embedding(vocab_size, d_model)
        self.book_emb = nn.Embedding(vocab_size, d_model)
        self.book_state = nn.Linear(40 * d_model, d_model)
        self.transformer_layers = nn.ModuleList([Block(d_model, num_heads) for _ in range(num_layers)]) # number of transformer layers
        self.ln_f = nn.LayerNorm(d_model)
        self.head = nn.Linear(d_model, vocab_size)

    def forward(self, x, book): 
        # x is originally of shape (B,T) and then after embeddings it becomes (B,T,C)
        batch, seq_len = x.shape
        # torch.arange(5)       # tensor([0, 1, 2, 3, 4])
        pos_emb = self.position_embedding(torch.arange(seq_len, device=x.device))
        tok_emb = self.token_embedding(x)
        book_emb = self.book_emb(book)
        book_emb = torch.flatten(book_emb, start_dim = 2)
        book_state_proj = self.book_state(book_emb)
        # book_state_proj is of shape (B, T // 4, d_model) -> (B, 128, d_model)
        # every book state needs to be broadcast to four contiguous message tokens because that is what constitutes a complete message
        # so we do repeat_interleave(4, dim=1) to get (B, 128, d_model) --> (B, 512, d_model) 
        # for [A,B,C], x.repeat_interleave(4, dim=…) --> [A,A,A,A, B,B,B,B, C,C,C,C] 
        book_state_proj = book_state_proj.repeat_interleave(4, dim=1)

        emb = pos_emb + tok_emb + book_state_proj
        for block in self.transformer_layers:
            emb = block(emb)
        out = self.head(self.ln_f(emb)) 
        return out







        

        
    
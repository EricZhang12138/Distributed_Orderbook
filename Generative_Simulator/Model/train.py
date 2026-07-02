import torch 
from dataset import LOBDataset
from generative_simulator import Simulator 
from pathlib import Path
from torch.utils.data import DataLoader, Dataset, Subset
import torch.nn as nn
import math

# hyperparameter: 
vocab_size = 131
epoch_num = 20



# dataset/dataloader     optimiser     
DATA_ROOT = Path(__file__).resolve().parent.parent / "Data"
book_tokens_path = DATA_ROOT / "book_tokens.npz"
message_tokens_path = DATA_ROOT / "message_tokens.npz"


def test():
    dataset = LOBDataset(message_tokens_path, book_tokens_path, window_size = 512)
    dataloader = torch.utils.data.DataLoader(dataset, batch_size = 128, shuffle = True)

    gen_simulator = Simulator(vocab_size = vocab_size, num_heads = 4, d_model = 128, num_layers = 4, max_seq_len = 512)
    for message, label, book_state in dataloader:
        pred = gen_simulator(message, book_state)
        loss_fn = nn.CrossEntropyLoss()
        print(f"prediction shape is {pred.shape}, label shape is {label.shape}")
        loss = loss_fn(pred.reshape(128*512, -1), label.reshape(128*512)).item()
        print(f"the loss of this single pass is {loss}")
        print(len(dataloader))
        break



def main(): 
    dataset = LOBDataset(message_tokens_path, book_tokens_path, window_size = 512)

    split = int(0.9 * len(dataset))
    full_size = int(len(dataset))

    train_set = Subset(dataset, range(0, split))
    val_set = Subset(dataset, range(split, full_size))

    train_loader = torch.utils.data.DataLoader(train_set, batch_size = 16, shuffle = True, num_workers = 2, pin_memory = True)
    val_loader = torch.utils.data.DataLoader(val_set, batch_size = 128, shuffle = False, num_workers = 2, pin_memory = True)

    # define the model itself
    gen_simulator = Simulator(vocab_size = vocab_size, num_heads = 4, d_model = 128, num_layers = 4, max_seq_len = 512)
    optimiser = torch.optim.AdamW(
    gen_simulator.parameters(),                                                                                                                              
    lr=3e-4,
    weight_decay=0.1,                                                                                                                                        
    betas=(0.9, 0.95),                                                                                                                                     
    )       

    total_steps = len(train_loader) * epoch_num
    warmup_steps = 1000
    def lr_lambda(step):                                                                                                                                         
      if step < warmup_steps:             
          return step / warmup_steps                                                                                                                           
      progress = (step - warmup_steps) / max(1, total_steps - warmup_steps)                                                                                    
      return max(0.1, 0.5 * (1 + math.cos(math.pi * progress)))
    scheduler = torch.optim.lr_scheduler.LambdaLR(optimiser, lr_lambda)  

    # Define the device and then move the model weights and the data onto the GPU
    device = "cuda" if torch.cuda.is_available() else "cpu"
    print(f"Training on device: {device}")
    gen_simulator = gen_simulator.to(device)

    loss_fn = nn.CrossEntropyLoss()

    best_loss = float("inf")
    for epoch in range(epoch_num):
        epoch_loss = 0.0
        gen_simulator.train()        # signal that this is for training only 
        for message, label, book_state in train_loader: 
            message = message.to(device, non_blocking = True)
            label = label.to(device, non_blocking = True)
            book_state = book_state.to(device, non_blocking = True)
            pred = gen_simulator(message, book_state)
            # the class dim has to be dim1
            # Originally pred is (B, T, V) where V is the vocab size, while label is (B, T)
            # But class dim are at dim 2 in (B, T, V) which is V
            # Therefore, we flatten them to be (B * T, V) and (B * T,)
            loss = loss_fn(pred.reshape(-1, vocab_size), label.reshape(-1)) # this is where the batch of data get aggregated and averaged 
            optimiser.zero_grad()
            loss.backward()
            torch.nn.utils.clip_grad_norm_(gen_simulator.parameters(), 1.0)
            optimiser.step()
            scheduler.step()
            epoch_loss += loss.item()
        epoch_loss = epoch_loss / len(train_loader)
        print(f"epoch {epoch} loss: {epoch_loss: .4f}")


        gen_simulator.eval()
        correct, total = 0, 0
        val_loss = 0.0
        with torch.no_grad(): 
            for message, label, book_state in val_loader: 
                message = message.to(device, non_blocking = True)
                label = label.to(device, non_blocking = True)
                book_state = book_state.to(device, non_blocking = True)
                pred = gen_simulator(message, book_state)
                correct += (label == pred.argmax(dim=-1)).sum().item()   # item() is converting the single valued tensor into normal python object tensor([6]) -> 6
                total += label.reshape(-1).size(0) 
                val_loss += loss_fn(pred.reshape(-1, vocab_size), label.reshape(-1)).item()  
            val_loss = val_loss / len(val_loader) 
            accuracy = correct / total
        print(f"evaluation accuracy {accuracy}")
    
        if val_loss < best_loss:
            best_loss = val_loss
            torch.save(gen_simulator.state_dict(), 'best.pt')

        # save every epoch 
        torch.save({
            "epoch": epoch,
            "model": gen_simulator.state_dict(), 
            "optimiser": optimiser.state_dict(),
            "best_val": best_loss
            }, "last.pt")


if __name__ == "__main__":
    test()
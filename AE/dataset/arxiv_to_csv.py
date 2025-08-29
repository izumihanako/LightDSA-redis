import pandas as pd
from datasets import load_dataset
import tqdm 
import sys

if len(sys.argv) < 2:
    print(" please provide the path to the arxiv dataset as a command line argument.")
    sys.exit(1) 
path_to_arxiv_dataset = sys.argv[1]
print(f"Reading arxiv dataset: {path_to_arxiv_dataset}") 

# load local dataset
ds = load_dataset( path_to_arxiv_dataset , "section")

# extract required fields (add error handling)
data = []
for split in ['train', 'validation', 'test']:
    for item in tqdm.tqdm(ds[split], desc=f"Processing {split} split"):
        try:
            data.append({
                'abstract': str(item['abstract']).replace('\n', '\\n').replace('"', '""'),
                'article': str(item['article']).replace('\n', '\\n').replace('"', '""'),
            })
        except KeyError:
            continue

# save to CSV(with proper escaping)
save_file_name = "arxiv_full.csv"
print(f"Saving to CSV file: {save_file_name}")
df = pd.DataFrame(data)
df.to_csv(
    save_file_name,
    index=False,
    encoding='utf-8',
    quoting=1,          # 1=QUOTE_MINIMAL, 2=QUOTE_ALL
    escapechar='\\',    # escape character
    quotechar='"',      # quote character
    doublequote=True    # double the quote character within fields
)
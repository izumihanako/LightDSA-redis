#!/bin/bash
set -e

# download arxiv dataset from huggingface
echo "Downloading arxiv dataset from huggingface..."
# install huggingface-cli
pip install -U huggingface_hub
export HF_ENDPOINT=https://hf-mirror.com
huggingface-cli download --repo-type dataset --resume-download ccdv/arxiv-summarization --local-dir arxiv
echo "Download complete."


# transform the dataset to csv format
echo "Transforming the dataset to csv format..."
python3 arxiv_to_csv.py arxiv/
echo "Done."
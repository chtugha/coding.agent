import os
import sentencepiece.sentencepiece_model_pb2 as sp_pb2
import modal

def main():
    input_path = "/Users/whisper/tokenizer/tokenizer_model/tokenizer_spm_32k_3.model"
    output_path = "/Users/whisper/tokenizer/tokenizer_model/tokenizer_spm_32k_de.model"
    
    model = sp_pb2.ModelProto()
    with open(input_path, "rb") as f:
        model.ParseFromString(f.read())
    
    base_size = len(model.pieces)
    german_chars = ["ä", "ö", "ü", "ß", "Ä", "Ö", "Ü"]
    for i, p in enumerate(model.pieces):
        if p.piece in german_chars:
            p.piece = f"unused_token_{i}"
    for c in german_chars:
        piece = model.pieces.add()
        piece.piece = c
        piece.score = 0.0
        piece.type = sp_pb2.ModelProto.SentencePiece.USER_DEFINED
        
    new_size = len(model.pieces)
    assert new_size == base_size + 7
    
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, "wb") as f:
        f.write(model.SerializeToString())
        
    vol_data = modal.Volume.from_name("moshi-german-data")
    with vol_data.batch_upload(force=True) as batch:
        batch.put_file(output_path, "/tokenizer_spm_32k_de.model")

if __name__ == "__main__":
    main()

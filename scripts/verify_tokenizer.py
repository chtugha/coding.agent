import sentencepiece as spm

def main():
    model_path = "/Users/whisper/tokenizer/tokenizer_model/tokenizer_spm_32k_de.model"
    sp = spm.SentencePieceProcessor()
    sp.Load(model_path)
    
    vocab_size = sp.GetPieceSize()
    print("Vocab size:", vocab_size)
    assert vocab_size == 32007, f"Expected size 32007, got {vocab_size}"
    
    german_chars = ["ä", "ö", "ü", "ß", "Ä", "Ö", "Ü"]
    for i, c in enumerate(german_chars):
        expected_idx = 32000 + i
        piece_id = sp.PieceToId(c)
        print(f"Char {c}: piece_id={piece_id}, expected={expected_idx}")
        assert piece_id == expected_idx, f"Expected {expected_idx} for {c}, got {piece_id}"
        
        piece_str = sp.IdToPiece(expected_idx)
        print(f"Id {expected_idx}: piece_str={piece_str!r}, expected={c!r}")
        assert piece_str == c, f"Expected {c!r} for id {expected_idx}, got {piece_str!r}"
        
    test_string = "Äpfel, Öfen und Überraschungen für die süßen Jungs."
    tokens = sp.EncodeAsPieces(test_string)
    print("Encoded pieces:", tokens)
    
    for t in tokens:
        assert t != "<unk>"
        assert t != " \u2047"
        
    print("Validation passed successfully!")

if __name__ == "__main__":
    main()

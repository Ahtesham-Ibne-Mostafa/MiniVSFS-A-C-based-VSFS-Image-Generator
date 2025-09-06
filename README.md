# MiniVSFS-A-C-based-VSFS-Image-Generator


markdown
# 🧠 MiniVSFS: Minimal Virtual Simple File System

MiniVSFS is a byte-accurate virtual file system built in C for educational and experimental use. It lets you create filesystem images, add files incrementally, and inspect binary layouts with full control. Designed for reproducibility, clarity, and low-level insight.

---

## ⚙️ Tools Included

| Tool         | Description                                      |
|--------------|--------------------------------------------------|
| `mkfs_builder` | Initializes a blank filesystem image             |
| `mkfs_adder`   | Adds a file to an existing image (one at a time) |

---

## 🚀 How to Use

### 1. 🔧 Build the Tools

```bash
1.  $gcc -O2 -std=c17 -Wall -Wextra mkfs_builder_skeleton.c -o mkfs_builder
    $gcc -Wall -Wextra -Werror -o mkfs_adder mkfs_adder_skeleton.c   

This compiles mkfs_builder and mkfs_adder into your working directory.

2. 📦 Create a Blank Filesystem Image
bash
./mkfs_builder --image out.img --size-kib 512 --inodes 256      
This generates a fresh out.img with an empty root directory.

3. 📁 Add Files to the Image (Step-by-Step)
Each run of mkfs_adder adds one file and produces a new image:

bash
./mkfs_adder --input out.img     --output out1.img --file file_9.txt
./mkfs_adder --input out1.img    --output out2.img --file file_13.txt
./mkfs_adder --input out2.img    --output out3.img --file file_20.txt
./mkfs_adder --input out3.img    --output out4.img --file file_34.txt
After the final step, out4.img contains all four files.

4. 🧪 Inspect the Final Image
Use hexdump or xxd to verify the binary layout:

bash
hexdump -C out4.img | less
Or use cmp to compare with a reference image:

bash
cmp out4.img reference.img
5. 🌀 Optional: Add Files In-Place (Overwrite Strategy)
This approach overwrites out.img after each addition:

bash
./mkfs_builder --output out.img

for f in file_9.txt file_13.txt file_20.txt file_34.txt; do
    ./mkfs_adder --input out.img --output out.img --file "$f"
done
🛠️ Developer Notes
All tools are written in C and follow strict standards compliance.

File addition preserves byte-level integrity and updates metadata correctly.

Images are validated using xxd, cmp, and adversarial test cases.

Struct packing and block layout are designed for clarity and reproducibility.

👨‍💻 Author
Built by Ahtesham, a systems programming enthusiast passionate about healthcare informatics, reproducible research, and robust tooling. This project reflects a commitment to clarity, correctness, and practical impact.

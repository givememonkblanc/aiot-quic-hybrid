"""Cache frozen CLIP-B/32 features for local image/caption pairs and prompts."""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
from PIL import Image

TEMPLATES = ('a photo of a {}', 'a photograph of a {}', 'an image of a {}', 'a picture of a {}')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--manifest', type=Path, required=True, help='JSONL: image path (relative to manifest), caption')
    p.add_argument('--classes', type=Path, required=True, help='JSON array of candidate class names')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--teacher', default='openai/clip-vit-base-patch32')
    p.add_argument('--device', default='cpu')
    args=p.parse_args()
    if args.out.exists(): p.error('output exists; choose a new file')
    import torch
    from transformers import CLIPModel, CLIPProcessor
    classes=json.loads(args.classes.read_text())
    if not classes or not all(isinstance(x,str) and x.strip() for x in classes) or len(set(classes))!=len(classes):
        p.error('classes must be unique nonempty strings')
    records=[json.loads(x) for x in args.manifest.read_text().splitlines() if x.strip()]
    if len(records)<2: p.error('contrastive training requires at least two image/caption pairs')
    model=CLIPModel.from_pretrained(args.teacher, use_safetensors=True).to(args.device).eval()
    processor=CLIPProcessor.from_pretrained(args.teacher)
    images=[]; image_features=[]; text_features=[]; hashes=[]
    with torch.no_grad():
        for r in records:
            path=(args.manifest.parent/r['image']).resolve()
            if not isinstance(r['caption'],str) or not r['caption'].strip(): raise ValueError('empty caption')
            with Image.open(path) as f: im=f.convert('RGB')
            inp=processor(text=[r['caption']], images=im, return_tensors='pt', padding=True, truncation=True).to(args.device)
            output=model(**inp)
            image_features.append(output.image_embeds.cpu().numpy()[0])
            text_features.append(output.text_embeds.cpu().numpy()[0])
            images.append(np.asarray(im.resize((128,128),Image.Resampling.BILINEAR),dtype=np.uint8))
            hashes.append(hashlib.sha256(path.read_bytes()).hexdigest())
        prototypes=[]
        for label in classes:
            inp=processor(text=[t.format(label) for t in TEMPLATES], return_tensors='pt', padding=True, truncation=True).to(args.device)
            e=model.get_text_features(**inp)
            if hasattr(e, 'pooler_output'): e=e.pooler_output  # Transformers 5
            e=e/e.norm(dim=-1,keepdim=True).clamp(min=1e-12)
            # Keep individual prompts: project before averaging in student space.
            prototypes.append(e.cpu().numpy())
    args.out.parent.mkdir(parents=True,exist_ok=True)
    teacher_hash=hashlib.sha256()
    for name,weight in sorted(model.state_dict().items()):
        teacher_hash.update(name.encode());teacher_hash.update(weight.detach().cpu().contiguous().numpy().tobytes())
    np.savez_compressed(args.out,images=np.stack(images),teacher_image=np.stack(image_features),
        teacher_text=np.stack(text_features),class_text=np.stack(prototypes),classes=np.asarray(classes),
        image_sha256=np.asarray(hashes),captions=np.asarray([r['caption'] for r in records]),
        teacher=np.asarray(args.teacher),teacher_state_sha256=np.asarray(teacher_hash.hexdigest()),
        teacher_revision=np.asarray(getattr(model.config,'_commit_hash',None) or 'local/unspecified'))
    print(f'Cached {len(images)} pairs, {len(classes)} classes: {args.out}')

if __name__=='__main__': main()

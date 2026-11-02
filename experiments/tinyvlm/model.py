"""Independent implementation of arXiv:2603.00136 equations 2--7.

Unspecified text student is resolved as a learned 512->256 projection of frozen
CLIP text features. This choice is shared by training and prototype export.
"""
import tensorflow as tf

DIMS = (16, 32, 64, 128, 256)


def build_models(seed=42, backbone_weights=None):
    tf.keras.utils.set_random_seed(seed)
    backbone = tf.keras.applications.MobileNetV2(input_shape=(128, 128, 3),
                 alpha=.35, include_top=False, weights=backbone_weights, pooling='avg')
    vision = tf.keras.Sequential([backbone, tf.keras.layers.Dense(256, name='embedding')], name='tinyvlm_vision')
    text = tf.keras.Sequential([tf.keras.Input((512,)), tf.keras.layers.Dense(256, use_bias=False)], name='text_projection')
    projection = tf.keras.Sequential([tf.keras.Input((256,)), tf.keras.layers.Dense(512, use_bias=False)], name='teacher_projection')
    return vision, text, projection


def normalize(x):
    return tf.math.l2_normalize(x, axis=-1, epsilon=1e-12)


def contrastive(image, text, temperature=.07):
    logits = tf.matmul(normalize(image), normalize(text), transpose_b=True) / temperature
    labels = tf.range(tf.shape(logits)[0])
    ce = tf.nn.sparse_softmax_cross_entropy_with_logits
    return (tf.reduce_mean(ce(labels=labels, logits=logits)) +
            tf.reduce_mean(ce(labels=labels, logits=tf.transpose(logits)))) / 2


def objective(image, text, teacher_image, teacher_text, projection):
    image, text = normalize(image), normalize(text)
    full = contrastive(image, text)
    emb = (tf.reduce_mean(tf.reduce_sum(tf.square(projection(image)-normalize(teacher_image)), axis=-1)) +
           tf.reduce_mean(tf.reduce_sum(tf.square(projection(text)-normalize(teacher_text)), axis=-1)))
    nested = tf.add_n([contrastive(image[:, :d], text[:, :d]) for d in DIMS]) / len(DIMS)
    return full + emb + .5*nested, dict(contrastive=full, embedding=emb, matryoshka=nested)

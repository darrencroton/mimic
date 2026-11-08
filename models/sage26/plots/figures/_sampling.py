"""
Random sampling shared by the SAGE26 scatter figures.

SAGE26's plotting script caps the galaxies a scatter draws at `dilute` with
`if(len(w) > dilute): w = sample(list(w), dilute)` (plotting/allresults-local.py, for example
:1571), drawing from one script-wide stream seeded once (:357). Each figure passes its own
random.Random(2222) as ``rng`` instead (the permitted deviation its docstring states) and its own
cap as ``dilute``, and cites the SAGE26 line of its call where it calls one of these helpers.
"""


def draw_sample(candidates, rng, dilute):
    """
    Return the galaxies to draw: all candidates, or a random sample of ``dilute`` of them.

    SAGE26: `if(len(w) > dilute): w = sample(list(w), dilute)`, with ``rng`` standing in for
    SAGE26's script-wide stream.
    """
    if len(candidates) > dilute:
        return rng.sample(list(candidates), dilute)
    return candidates


def draw_sorted_sample(candidates, rng, dilute):
    """
    Return the galaxies to draw: all candidates, or ``dilute`` of them in sorted order.

    SAGE26 allresults-local.py:1861-1864: `if len(w) > dilute: sel = sorted(sample(list(w),
    dilute)) else: sel = w`, with ``rng`` standing in for SAGE26's script-wide stream.
    """
    if len(candidates) > dilute:
        return sorted(rng.sample(list(candidates), dilute))
    return candidates


def draw_sample_positions(candidates, rng, dilute):
    """
    Return the positions SAGE26 draws: all candidates, or ``dilute`` random positions.

    SAGE26 allresults-local.py:1738: `if(len(w2) > dilute): w2 = sample(list(range(len(w2))),
    dilute)`, with ``rng`` standing in for SAGE26's script-wide stream. The positions are drawn
    from range(len(w2)), not from w2 itself.
    """
    if len(candidates) > dilute:
        return rng.sample(list(range(len(candidates))), dilute)
    return candidates

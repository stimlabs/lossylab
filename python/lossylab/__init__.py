"""Model, apply and inspect the processing history of images and video frames."""

from . import _lossylab as _native

# Re-export every public name the compiled extension provides. Deliberately
# not a hand-curated list: that drifts the moment a binding is added or
# removed in python/src/bind_*.cpp without this file being updated to match.
__all__ = [name for name in dir(_native) if not name.startswith("_")]
globals().update({name: getattr(_native, name) for name in __all__})

del _native

import itertools
import numpy as np
from typing import Any, Callable, Tuple

# ================== Decorators ===================
class TaskInstance:
    def __init__(self, func, grid_dims, tile_sizes, instanceIdx, top_flag, call_args, call_kwargs):
        self.func = func
        self.grid_dims = grid_dims
        self.tile_sizes = tile_sizes
        self.instanceIdx = instanceIdx
        self.top_flag = top_flag
        self.call_args = call_args
        self.call_kwargs = call_kwargs

class TaskTileWrapper:
    def __init__(self, func: Callable, run=True):
        self.func = func
        self.grid_dims = None
        self.tile_sizes = None
        self.instanceIdx = 0
        self.run = run

    def __getitem__(self, args):
        if isinstance(args, tuple) and all(not isinstance(x, tuple) for x in args):
            args = (args,)
        if len(args) >= 1:
            self.grid_dims = args[0]  # Grid dimensions (e.g., (4, 4) for 2D tiling)
        if len(args) >= 2:
            self.tile_sizes = args[1]  # Tile sizes (e.g., (32, 32))
        if len(args) == 3:
            self.instanceIdx = args[2] # Mark the number of instance
        return self

    def __call__(self, *call_args, **call_kwargs):
        instance = TaskInstance(self.func, self.grid_dims, self.tile_sizes, 
                                self.instanceIdx, False, call_args, call_kwargs)
        if not self.grid_dims:
            if self.run:
                self.func(*call_args)
            return instance
        if isinstance(self.grid_dims, int):
            self.grid_dims = (self.grid_dims,)
        if self.tile_sizes:
            call_kwargs['TSizes'] = self.tile_sizes  # Add tuple of sizes
        # Generate all tile index combinations (e.g., (i, j) for a 2D grid)
        if self.run:
            for idx in itertools.product(*(range(g) for g in self.grid_dims)):
                call_kwargs['IVs'] = idx  # Add tuple of indices
                self.func(*call_args, **call_kwargs)
        return instance

def task_tile(run_flag=True):  
    """Decorator to wrap the function for tiles."""
    def decorator(func: Callable):
        return TaskTileWrapper(func, run_flag)
    return decorator

class TaskKernelWrapper:
    def __init__(self, func: Callable, external_kernel = None, para = []):
        self.func = func
        self.externKrnl = external_kernel
        self.para = para
    def __call__(self, *call_args, **call_kwargs):
        self.func(*call_args, **call_kwargs)

def task_kernel(*, external_path=None, para = []):  
    """Decorator to wrap the function for single AIE."""
    def decorator(func: Callable):
        return TaskKernelWrapper(func, external_path, para)
    return decorator

class TaskTopWrapper:
    def __init__(self, func: Callable):
        self.func = func

    def __call__(self, *call_args, **call_kwargs):
        result = self.func(*call_args, **call_kwargs)

        """
        Currently this part will be handled by aries.gen_sim api explictly
        """
        # # Ensure result is a tuple (handles cases where a single value is returned)
        # results = result if isinstance(result, tuple) else (result,)        
        # for i, arg in enumerate(call_args):
        #     for value in results:
        #         if id(value) == id(arg):
        #             arg = value
        #             break
        #     np_arg = np.array(arg)
        #     dtype = np_arg.dtype
        #     if dtype == np.int8 or dtype == np.int16 or dtype == np.int32:
        #         fmt = '%d'
        #     elif dtype == np.float32 or dtype == np.float64:
        #         fmt = '%.6f'
        #     else:
        #         fmt = '%s'
        #     np.savetxt(f'data{i}.sim', np_arg, fmt=fmt)
        return result

def task_top():  
    """Decorator to wrap the function for top function."""
    def decorator(func: Callable):
        return TaskTopWrapper(func)
    return decorator
  
# ======= Reduction range ========
class ReductionRange:
    def __init__(self, start, stop, step=1):
        if stop is None:
            start, stop = 0, start
        self.range = range(start, stop, step)
        
    def __iter__(self):
        return iter(self.range)

# Helper function to create a ReductionRange
def reduction_range(start, stop, step=1):
    return ReductionRange(start, stop, step)
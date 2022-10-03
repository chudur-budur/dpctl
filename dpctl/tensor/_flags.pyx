#                       Data Parallel Control (dpctl)
#
#  Copyright 2020-2022 Intel Corporation
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  you may not use this file except in compliance with the License.
#  You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.

# distutils: language = c++
# cython: language_level=3
# cython: linetrace=True

from dpctl.tensor._usmarray cimport (
    USM_ARRAY_C_CONTIGUOUS,
    USM_ARRAY_F_CONTIGUOUS,
    USM_ARRAY_WRITEABLE,
)


class Flags:

    def __init__(self, arr, flags):
        self.arr_ = arr
        self.flags_ = flags

    @property
    def flags(self):
        return self.flags_

    @property
    def c_contiguous(self):
        return ((self.flags_ & USM_ARRAY_C_CONTIGUOUS)
                == USM_ARRAY_C_CONTIGUOUS)

    @property
    def f_contiguous(self):
        return ((self.flags_ & USM_ARRAY_F_CONTIGUOUS)
                == USM_ARRAY_F_CONTIGUOUS)

    @property
    def writable(self):
        return False if ((self.flags & USM_ARRAY_WRITEABLE)
                        == USM_ARRAY_WRITEABLE) else True

    @property
    def forc(self):
        return True if (((self.flags_ & USM_ARRAY_F_CONTIGUOUS)
                        == USM_ARRAY_F_CONTIGUOUS)
                        or ((self.flags_ & USM_ARRAY_C_CONTIGUOUS)
                        == USM_ARRAY_C_CONTIGUOUS)) else False

    @property
    def fnc(self):
        return True if (((self.flags_ & USM_ARRAY_F_CONTIGUOUS)
                        == USM_ARRAY_F_CONTIGUOUS)
                        and not ((self.flags_ & USM_ARRAY_C_CONTIGUOUS)
                        == USM_ARRAY_C_CONTIGUOUS)) else False

    @property
    def contiguous(self):
        return self.forc

    def __getitem__(self, name):
        if name in ["C_CONTIGUOUS", "C"]:
            return self.c_contiguous
        elif name in ["F_CONTIGUOUS", "F"]:
            return self.f_contiguous
        elif name == "WRITABLE":
            return self.writable
        elif name == "CONTIGUOUS":
            return self.forc

    def __repr__(self):
        out = []
        for name in "C_CONTIGUOUS", "F_CONTIGUOUS", "WRITABLE":
            out.append("  {} : {}".format(name, self[name]))
        return '\n'.join(out)

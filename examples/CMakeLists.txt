# SPDX-License-Identifier: Apache-2.0
#
# Examples are consumers, not part of the library: they link smply::smply (and
# smply::util where they need marshalling) and never the internal target.
#
# They are smply's own code, so the strict warning set and the format and lint
# gates apply to them -- but PRIVATE, so nothing about that reaches a real
# consumer copying the code out.

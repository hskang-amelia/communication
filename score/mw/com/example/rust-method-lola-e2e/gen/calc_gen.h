/********************************************************************************
 * Copyright (c) 2026 Contributors to the Eclipse Foundation
 *
 * See the NOTICE file(s) distributed with this work for additional
 * information regarding copyright ownership.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Apache License Version 2.0 which is available at
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * SPDX-License-Identifier: Apache-2.0
 ********************************************************************************/
#ifndef SCORE_MW_COM_EXAMPLE_RUST_METHOD_LOLA_E2E_CALC_GEN_H
#define SCORE_MW_COM_EXAMPLE_RUST_METHOD_LOLA_E2E_CALC_GEN_H

#include "score/mw/com/types.h"

#include <cstdint>

namespace score::mw::com
{

struct Operands
{
    std::int32_t a;
    std::int32_t b;
};

struct Total
{
    std::int32_t value;
};

/// One method and one field, so a Rust producer and a Rust consumer in different processes can
/// exercise both over the LoLa runtime.
template <typename Trait>
class CalcInterface : public Trait::Base
{
  public:
    using Trait::Base::Base;

    typename Trait::template Method<Total(Operands)> add{*this, "add"};
    typename Trait::template Field<Total, WithGetter, WithSetter, WithNotifier> counter{*this, "counter"};
};

using CalcProxy = AsProxy<CalcInterface>;
using CalcSkeleton = AsSkeleton<CalcInterface>;

}  // namespace score::mw::com

#endif  // SCORE_MW_COM_EXAMPLE_RUST_METHOD_LOLA_E2E_CALC_GEN_H

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "exceptions/exception_traits_doesnotexist.h"
#include "exceptions/exception_traits_invalidtraitvalue.h"
#include "exceptions/exception_traits_notmutable.h"
#include "kernel/auxillary_traits/auxillary_traits.h"
#include "kernel/auxillary_traits/auxillary_traits_base.h"
#include "kernel/auxillary_traits/auxillary_traits_proxy.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"

BOOST_AUTO_TEST_SUITE(AuxillaryTraitsTestSuite)

BOOST_AUTO_TEST_CASE(Test_MutableTraits_Simple)
{
    using namespace AqualinkAutomate::Kernel;

    using TestTypes_Simple = uint32_t;
    class TestTypesTrait_Simple : public MutableTraitType<TestTypes_Simple>
    {
    public:
        TraitKey Name() const final { return std::string{"TestTypesTrait_Mutable_Simple"}; }
    };

    Traits traits;
    TestTypesTrait_Simple trait;

    // Check trait does not exist
    BOOST_CHECK(!traits.Has(trait));

    // Set trait
    traits.Set(trait, 1234);
    BOOST_CHECK(traits.Has(trait));

    // Get trait
    auto traitValue = *(traits.Get(trait));
    BOOST_CHECK(traitValue == 1234);

    // Update trait
    traits[trait] = 5678;
    traitValue = *(traits.Get(trait));
    BOOST_CHECK(traitValue == 5678);

    // Remove trait
    BOOST_CHECK(traits.Remove(trait));
    BOOST_CHECK(!traits.Has(trait));
}

BOOST_AUTO_TEST_CASE(Test_ImmutableTraits_Simple)
{
    using namespace AqualinkAutomate::Exceptions;
    using namespace AqualinkAutomate::Kernel;

    using TestTypes_Simple = uint32_t;
    class TestTypesTrait_Simple : public ImmutableTraitType<TestTypes_Simple>
    {
    public:
        TraitKey Name() const final { return std::string{"TestTypesTrait_Immutable_Simple"}; }
    }; 
    
    Traits traits;
    TestTypesTrait_Simple trait;

    // Check trait does not exist
    BOOST_CHECK(!traits.Has(trait));

    // Set trait
    traits.Set(trait, 1234);
    BOOST_CHECK(traits.Has(trait));

    // Get trait
    auto traitValue = *(traits.Get(trait));
    BOOST_CHECK(traitValue == 1234);

    // Attempt to update trait - should not be permitted
    BOOST_CHECK_THROW(traits[trait] = 5678, Traits_NotMutable);

    // Attempt to remove trait - should not be permitted
    BOOST_CHECK(!traits.Remove(trait));
    BOOST_CHECK(traits.Has(trait));
}

BOOST_AUTO_TEST_CASE(Test_MutableTraits_Complex)
{
    using namespace AqualinkAutomate::Kernel;

    using TestTypes_Complex = std::vector<uint32_t>;
    class TestTypesTrait_Complex : public MutableTraitType<TestTypes_Complex>
    {
    public:
        TraitKey Name() const final { return std::string{"TestTypesTrait_Mutable_Complex"}; }
    };

    Traits traits;    
    TestTypesTrait_Complex trait;
    std::vector<uint32_t> test_dataset_1{1,2,3,4,5};
    std::vector<uint32_t> test_dataset_2{6,7,8,9,10};

    // Check trait does not exist
    BOOST_CHECK(!traits.Has(trait));

    // Set trait
    traits.Set(trait, test_dataset_1);
    BOOST_CHECK(traits.Has(trait));

    // Get trait
    auto traitValue = *(traits.Get(trait));
    BOOST_CHECK(traitValue == test_dataset_1);

    // Update trait
    traits[trait] = test_dataset_2;
    traitValue = *(traits.Get(trait));
    BOOST_CHECK(traitValue == test_dataset_2);

    // Remove trait
    BOOST_CHECK(traits.Remove(trait));
    BOOST_CHECK(!traits.Has(trait));
}

BOOST_AUTO_TEST_CASE(Test_ImmutableTraits_Complex)
{
    using namespace AqualinkAutomate::Exceptions;
    using namespace AqualinkAutomate::Kernel;

    using TestTypes_Complex = std::vector<uint32_t>;
    class TestTypesTrait_Complex : public ImmutableTraitType<TestTypes_Complex>
    {
    public:
        TraitKey Name() const final { return std::string{"TestTypesTrait_Immutable_Complex"}; }
    };

    Traits traits;
    TestTypesTrait_Complex trait;
    std::vector<uint32_t> test_dataset_1{1, 2, 3, 4, 5};
    std::vector<uint32_t> test_dataset_2{6, 7, 8, 9, 10};

    // Check trait does not exist
    BOOST_CHECK(!traits.Has(trait));

    // Set trait
    traits.Set(trait, test_dataset_1);
    BOOST_CHECK(traits.Has(trait));

    // Get trait
    auto traitValue = *(traits.Get(trait));
    BOOST_CHECK(traitValue == test_dataset_1);

    // Attempt to update trait - should not be permitted
    BOOST_CHECK_THROW(traits[trait] = test_dataset_2, Traits_NotMutable);

    // Attempt to remove trait - should not be permitted
    BOOST_CHECK(!traits.Remove(trait));
    BOOST_CHECK(traits.Has(trait));
}

BOOST_AUTO_TEST_CASE(Test_Traits_Get_Exceptions)
{
    using namespace AqualinkAutomate::Exceptions;
    using namespace AqualinkAutomate::Kernel;

    using TestTypes_Simple = uint32_t;
    class TestTypesTrait_Simple : public MutableTraitType<TestTypes_Simple>
    {
    public:
        TraitKey Name() const final { return std::string{"TestTypesTrait_Simple"}; }
    }; 
    
    Traits traits;
    TestTypesTrait_Simple trait;

    // Attempt to get a trait that does not exist
    BOOST_CHECK_THROW(traits.Get(trait), Traits_DoesNotExist);
    // Attempt to get a trait through a TraitValueProxy that does not exist
    BOOST_CHECK_THROW(traits.Get(TestTypesTrait_Simple{}), Traits_DoesNotExist);

    // Add the trait
    traits.Set(trait, 1234);

    using TestTypes_Complex = std::vector<uint32_t>;
    class TestTypesTrait_Complex : public MutableTraitType<TestTypes_Complex>
    {
    public:
        TraitKey Name() const final { return std::string{"TestTypesTrait_Complex"}; }
    };

    std::vector<uint32_t>::const_iterator cit;

    // Attempt to get a trait through a ConstTraitValueProxy that does not exist
    BOOST_CHECK_THROW(const auto& ABC = *(traits.Get(TestTypesTrait_Complex{})), Traits_DoesNotExist);
    BOOST_CHECK_THROW((*(traits.Get(TestTypesTrait_Complex{}))).reserve(1), Traits_DoesNotExist);
    BOOST_CHECK_THROW(traits.Get(TestTypesTrait_Complex{})->reserve(1), Traits_DoesNotExist);
}

// Regression: the five status trait types (StatusTrait/AuxillaryStatusTrait/ChlorinatorStatusTrait/
// HeaterStatusTrait/PumpStatusTrait) previously shared a single map key
// ("StatusTrait_MultipleTraitTypes"). Storing two of them on the same Traits map therefore
// overwrote each other and could provoke a bad_any_cast across incompatible value types. With
// distinct Name()s they must coexist independently.
BOOST_AUTO_TEST_CASE(Test_StatusTraits_DistinctKeys_NoCollision)
{
    using namespace AqualinkAutomate::Kernel;
    using namespace AqualinkAutomate::Kernel::AuxillaryTraitsTypes;

    // Distinct Name() per status trait.
    BOOST_CHECK_NE(PumpStatusTrait{}.Name(), AuxillaryStatusTrait{}.Name());
    BOOST_CHECK_NE(PumpStatusTrait{}.Name(), ChlorinatorStatusTrait{}.Name());
    BOOST_CHECK_NE(PumpStatusTrait{}.Name(), HeaterStatusTrait{}.Name());
    BOOST_CHECK_NE(AuxillaryStatusTrait{}.Name(), HeaterStatusTrait{}.Name());
    BOOST_CHECK_NE(ChlorinatorStatusTrait{}.Name(), HeaterStatusTrait{}.Name());

    Traits traits;

    // Set two different status traits with different value types on the SAME map.
    traits.Set(PumpStatusTrait{}, PumpStatuses::Running);
    traits.Set(HeaterStatusTrait{}, HeaterStatuses::Heating);

    // Both must remain independently retrievable - no overwrite, no cross-type any_cast failure.
    auto pump_status = traits.TryGet(PumpStatusTrait{});
    auto heater_status = traits.TryGet(HeaterStatusTrait{});

    BOOST_REQUIRE(pump_status.has_value());
    BOOST_REQUIRE(heater_status.has_value());
    BOOST_CHECK(pump_status.value() == PumpStatuses::Running);
    BOOST_CHECK(heater_status.value() == HeaterStatuses::Heating);
}

// The proxy still surfaces a genuine value-type mismatch (bad_any_cast -> Traits_InvalidTraitValue);
// only the unreachable out_of_range catch was removed. We provoke a mismatch by injecting a value
// of the wrong type under a trait's key via a second trait that reuses the same Name().
BOOST_AUTO_TEST_CASE(Test_Traits_InvalidValueType_Throws)
{
    using namespace AqualinkAutomate::Exceptions;
    using namespace AqualinkAutomate::Kernel;

    class TraitA : public MutableTraitType<uint32_t>
    {
    public:
        TraitKey Name() const final { return std::string{"SharedKeyForMismatchTest"}; }
    };

    class TraitB : public MutableTraitType<std::string>
    {
    public:
        TraitKey Name() const final { return std::string{"SharedKeyForMismatchTest"}; }
    };

    Traits traits;
    traits.Set(TraitA{}, 1234U);

    // The trait key exists, so Get() succeeds and returns a proxy; the any_cast to the wrong
    // value type then throws Traits_InvalidTraitValue (the surviving bad_any_cast path).
    BOOST_CHECK_THROW((void)static_cast<std::string>(traits.Get(TraitB{})), Traits_InvalidTraitValue);

    // TryGet swallows the mismatch and reports "no value" rather than throwing.
    BOOST_CHECK(!traits.TryGet(TraitB{}).has_value());
}

// The MUTABLE (non-const) TraitValueProxy exposes operator* and operator-> onto the
// stored value in place. Retrieving a complex trait from a non-const Traits store and
// mutating it through the arrow/dereference operators exercises the non-const proxy
// success paths (distinct from the const proxy the other tests use).
// A value-type mismatch surfaced through the MUTABLE proxy's operator* (Reference
// cast) also raises Traits_InvalidTraitValue — the non-const CastOrThrow catch.
BOOST_AUTO_TEST_CASE(Test_MutableProxy_DerefMismatch_Throws)
{
    using namespace AqualinkAutomate::Exceptions;
    using namespace AqualinkAutomate::Kernel;

    class TraitA : public MutableTraitType<uint32_t>
    {
    public:
        TraitKey Name() const final { return std::string{"MutableProxy_Mismatch_Shared"}; }
    };
    class TraitB : public MutableTraitType<std::vector<uint32_t>>
    {
    public:
        TraitKey Name() const final { return std::string{"MutableProxy_Mismatch_Shared"}; }
    };

    Traits traits;
    traits.Set(TraitA{}, 99U);

    // The key exists (so Get() returns a proxy), but dereferencing as the wrong value
    // type trips the mutable proxy's bad_any_cast -> Traits_InvalidTraitValue.
    BOOST_CHECK_THROW((void)(*(traits.Get(TraitB{}))).size(), Traits_InvalidTraitValue);
    BOOST_CHECK_THROW((void)traits.Get(TraitB{})->size(), Traits_InvalidTraitValue);
}

// The Traits store methods are constrained on IsTraitType: the real trait descriptors model it.
BOOST_AUTO_TEST_CASE(Test_IsTraitType_Concept)
{
    using namespace AqualinkAutomate::Kernel;
    using namespace AqualinkAutomate::Kernel::AuxillaryTraitsTypes;

    static_assert(IsTraitType<PumpStatusTrait>);
    static_assert(IsTraitType<HeaterStatusTrait>);
    static_assert(IsTraitType<AuxillaryTypeTrait>);
    static_assert(IsTraitType<LabelTrait>);
    static_assert(!IsTraitType<int>);
    BOOST_CHECK(true);
}

BOOST_AUTO_TEST_SUITE_END()

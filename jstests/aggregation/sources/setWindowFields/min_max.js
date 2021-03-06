/**
 * Test that $min/max works as a window function.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/window_function_helpers.js");

const featureEnabled =
    assert.commandWorked(db.adminCommand({getParameter: 1, featureFlagWindowFunctions: 1}))
        .featureFlagWindowFunctions.value;
if (!featureEnabled) {
    jsTestLog("Skipping test because the window function feature flag is disabled");
    return;
}

const coll = db[jsTestName()];
coll.drop();

// Create a collection of tickers and prices.
const nDocsPerTicker = 10;
seedWithTickerData(coll, nDocsPerTicker);

// Run the suite of partition and bounds tests against the $min function.
testAccumAgainstGroup(coll, "$min", nDocsPerTicker);

// Run the suite of partition and bounds tests against the $max function.
testAccumAgainstGroup(coll, "$max", nDocsPerTicker);

// Test the behavior of min/max over a non-numeric field. Note that $min and $max order values by
// type per the BSON spec, so there will always be a value to return.
let results =
    coll.aggregate([
            {$addFields: {str: "hiya"}},
            {
                $setWindowFields: {
                    sortBy: {ts: 1},
                    output: {
                        minStr: {$min: {input: "$str", documents: ["unbounded", "current"]}},
                        maxStr: {$max: {input: "$str", documents: ["unbounded", "current"]}},
                    }
                }
            }
        ])
        .toArray();
for (let index = 0; index < results.length; index++) {
    assert.eq("hiya", results[index].minStr);
    assert.eq("hiya", results[index].maxStr);
}
})();

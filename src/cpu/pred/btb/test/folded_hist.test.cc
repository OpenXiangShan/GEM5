#include <gtest/gtest.h>
#include "cpu/pred/btb/folded_hist.hh"

using namespace gem5::branch_prediction::btb_pred;

class FoldedHistTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// Test basic initialization
TEST_F(FoldedHistTest, BasicInit) {
    // Create a folded history with histLen=8, foldedLen=4, maxShamt=2
    FoldedHist hist(8, 4, 2);
    
    // Initial state should be all zeros
    auto folded = hist.getAsBitset();
    EXPECT_EQ(folded.size(), 4);  // foldedLen = 4
    EXPECT_EQ(folded.count(), 0); // All bits should be 0
}

// Test basic update with single bit
TEST_F(FoldedHistTest, BasicUpdate) {
    FoldedHist hist(8, 4, 2);
    
    // Create a global history register (all zeros)
    boost::dynamic_bitset<> ghr(8, 0);
    
    // Update with taken branch (shamt=1)
    hist.update(ghr, 1, true);
    
    // After update, bit 0 should be 1, others 0
    auto folded = hist.getAsBitset();
    EXPECT_TRUE(folded[0]);
    EXPECT_FALSE(folded[1]);
    EXPECT_FALSE(folded[2]);
    EXPECT_FALSE(folded[3]);
}

// Test multiple updates
TEST_F(FoldedHistTest, MultipleUpdates) {
    FoldedHist hist(8, 4, 2);
    boost::dynamic_bitset<> ghr(8, 0);
    boost::dynamic_bitset<> temp_ghr(8, 0);
    
    // Simulate a sequence: taken -> not taken -> taken
    std::vector<bool> sequence = {true, false, true};
    
    for (bool taken : sequence) {
        // Update temp_ghr
        temp_ghr <<= 1;
        temp_ghr[0] = taken;
        
        // Update folded history
        hist.update(temp_ghr, 1, taken);
    }
    
    // Verify final state
    auto folded = hist.getAsBitset();
    EXPECT_TRUE(folded[0]);   // Latest update (taken)
    EXPECT_FALSE(folded[1]);  // Second update (not taken)
    EXPECT_TRUE(folded[2]);   // First update (taken)
    EXPECT_FALSE(folded[3]);  // Initial state
    
    // Build final GHR for verification
    ghr[0] = true;  // Latest
    ghr[1] = false;
    ghr[2] = true;
    EXPECT_NO_THROW(hist.check(ghr));
}

// Test recovery functionality
TEST_F(FoldedHistTest, BasicRecover) {
    FoldedHist hist1(8, 4, 2);
    FoldedHist hist2(8, 4, 2);
    
    // Create a test pattern
    boost::dynamic_bitset<> ghr(8);
    ghr[0] = 1; ghr[3] = 1; ghr[6] = 1;
    
    // Initialize hist1 properly
    boost::dynamic_bitset<> temp_ghr(8, 0);
    for (int i = 7; i >= 0; i--) {
        temp_ghr <<= 1;
        if (i < 7) temp_ghr[0] = ghr[i+1];
        hist1.update(temp_ghr, 1, ghr[i]);
    }
    
    // Recover hist2 from hist1
    hist2.recover(hist1);
    
    // Both histories should be identical
    EXPECT_EQ(hist1.get(), hist2.get());
    
    // Both should pass the check
    EXPECT_NO_THROW(hist1.check(ghr));
    EXPECT_NO_THROW(hist2.check(ghr));
}

// Test check functionality
TEST_F(FoldedHistTest, BasicCheck) {
    FoldedHist hist(8, 4, 2);
    
    // Create a global history register
    boost::dynamic_bitset<> ghr(8, 0);
    ghr[0] = 1;  // Set lowest bit
    
    // Update history
    hist.update(ghr, 1, true);
    
    // Check should pass (not throw assertion)
    EXPECT_NO_THROW(hist.check(ghr));
}

// Test check functionality with various XOR patterns
TEST_F(FoldedHistTest, CheckXORPatterns) {
    // Test 1: Basic alternating pattern
    {
        FoldedHist hist(8, 4, 2);
        boost::dynamic_bitset<> ghr(8);
        // Set alternating pattern: 1,0,1,0,1,0,1,0
        for (int i = 0; i < 8; i += 2) {
            ghr[i] = 1;
        }
        
        // Initialize folded history by updating with each bit
        for (int i = 7; i >= 0; i--) {
            hist.update(ghr, 1, ghr[i]);
        }
        
        // Expected folded result after XORing:
        // Position 0: 1 XOR 1 = 0 (bits 0,4)
        // Position 1: 0 XOR 0 = 0 (bits 1,5)
        // Position 2: 1 XOR 1 = 0 (bits 2,6)
        // Position 3: 0 XOR 0 = 0 (bits 3,7)
        EXPECT_NO_THROW(hist.check(ghr));
        auto folded = hist.getAsBitset();
        EXPECT_FALSE(folded[0]);
        EXPECT_FALSE(folded[1]);
        EXPECT_FALSE(folded[2]);
        EXPECT_FALSE(folded[3]);
    }

    // Test 2: All ones pattern
    {
        FoldedHist hist(8, 4, 2);
        boost::dynamic_bitset<> ghr(8);
        ghr.set(); // Set all bits to 1
        
        // Initialize folded history
        for (int i = 7; i >= 0; i--) {
            hist.update(ghr, 1, ghr[i]);
        }
        
        // Expected folded result after XORing:
        // Position 0: 1 XOR 1 = 0 (bits 0,4)
        // Position 1: 1 XOR 1 = 0 (bits 1,5)
        // Position 2: 1 XOR 1 = 0 (bits 2,6)
        // Position 3: 1 XOR 1 = 0 (bits 3,7)
        EXPECT_NO_THROW(hist.check(ghr));
        auto folded = hist.getAsBitset();
        EXPECT_FALSE(folded[0]);
        EXPECT_FALSE(folded[1]);
        EXPECT_FALSE(folded[2]);
        EXPECT_FALSE(folded[3]);
    }

    // Test 3: First half ones, second half zeros
    {
        FoldedHist hist(8, 4, 2);
        boost::dynamic_bitset<> ghr(8);
        // Set pattern: 1,1,1,1,0,0,0,0
        for (int i = 0; i < 4; i++) {
            ghr[i] = 1;
        }
        
        // Initialize folded history
        for (int i = 7; i >= 0; i--) {
            hist.update(ghr, 1, ghr[i]);
        }
        
        // Expected folded result after XORing:
        // Position 0: 1 XOR 0 = 1 (bits 0,4)
        // Position 1: 1 XOR 0 = 1 (bits 1,5)
        // Position 2: 1 XOR 0 = 1 (bits 2,6)
        // Position 3: 1 XOR 0 = 1 (bits 3,7)
        EXPECT_NO_THROW(hist.check(ghr));
        auto folded = hist.getAsBitset();
        EXPECT_TRUE(folded[0]);
        EXPECT_TRUE(folded[1]);
        EXPECT_TRUE(folded[2]);
        EXPECT_TRUE(folded[3]);
    }

    // Test 4: Complex pattern
    {
        FoldedHist hist(8, 4, 2);
        boost::dynamic_bitset<> ghr(8);
        // Set pattern: 1,1,0,0,1,0,1,0
        ghr[0] = 1; ghr[1] = 1; ghr[4] = 1; ghr[6] = 1;
        
        // Initialize folded history
        for (int i = 7; i >= 0; i--) {
            hist.update(ghr, 1, ghr[i]);
        }
        
        // Expected folded result after XORing:
        // Position 0: 1 XOR 1 = 0 (bits 0,4)
        // Position 1: 1 XOR 0 = 1 (bits 1,5)
        // Position 2: 0 XOR 1 = 1 (bits 2,6)
        // Position 3: 0 XOR 0 = 0 (bits 3,7)
        EXPECT_NO_THROW(hist.check(ghr));
        auto folded = hist.getAsBitset();
        EXPECT_FALSE(folded[0]);
        EXPECT_TRUE(folded[1]);
        EXPECT_TRUE(folded[2]);
        EXPECT_FALSE(folded[3]);
    }

    // Test 5: Odd length history
    {
        FoldedHist hist(7, 3, 2);
        boost::dynamic_bitset<> ghr(7);
        // Set pattern: 1,1,0,1,0,1,1 (from LSB to MSB)
        ghr[0] = 1; ghr[1] = 1; ghr[3] = 1; ghr[5] = 1; ghr[6] = 1;
        
        std::cout << "\n=== Debug Info for Odd Length History Test ===\n";
        std::cout << "Initial GHR: " << ghr << std::endl;
        
        // Create a temporary GHR to build history
        boost::dynamic_bitset<> temp_ghr(7, 0);
        
        // Build history from oldest to newest bit
        for (int i = 6; i >= 0; i--) {
            // Update temp_ghr to reflect the state at this point
            temp_ghr <<= 1;
            if (i < 6) {  // Don't shift in a bit for the first update
                temp_ghr[0] = ghr[i+1];
            }
            
            std::cout << "\nStep " << (6-i) << ":\n";
            std::cout << "Current temp_ghr before update: " << temp_ghr << std::endl;
            std::cout << "Adding bit: " << ghr[i] << std::endl;
            
            // Update folded history
            hist.update(temp_ghr, 1, ghr[i]);
            
            auto current_folded = hist.getAsBitset();
            std::cout << "Current folded history: " << current_folded << std::endl;
        }
        
        std::cout << "\nFinal state:\n";
        std::cout << "Final GHR: " << ghr << std::endl;
        auto final_folded = hist.getAsBitset();
        std::cout << "Final folded history: " << final_folded << std::endl;
        
        // Calculate expected idealFolded manually for verification
        boost::dynamic_bitset<> idealFolded(3, 0);
        for (int i = 0; i < 7; i++) {
            idealFolded[i % 3] ^= ghr[i];
        }
        std::cout << "Expected idealFolded: " << idealFolded << std::endl;
        
        // Print bit-by-bit XOR calculation
        std::cout << "\nDetailed XOR calculation:\n";
        std::cout << "Position 0 (bits 0,3,6): " << ghr[0] << " XOR " << ghr[3] << " XOR " << ghr[6] << std::endl;
        std::cout << "Position 1 (bits 1,4): " << ghr[1] << " XOR " << ghr[4] << std::endl;
        std::cout << "Position 2 (bits 2,5): " << ghr[2] << " XOR " << ghr[5] << std::endl;
        
        EXPECT_NO_THROW(hist.check(ghr));
        
        // Add detailed failure messages
        EXPECT_TRUE(final_folded[0]) << "Position 0 should be 1 (XOR of bits 0,3,6: " 
                                      << ghr[0] << "," << ghr[3] << "," << ghr[6] << ")";
        EXPECT_TRUE(final_folded[1]) << "Position 1 should be 1 (XOR of bits 1,4: "
                                      << ghr[1] << "," << ghr[4] << ")";
        EXPECT_TRUE(final_folded[2]) << "Position 2 should be 1 (XOR of bits 2,5: "
                                      << ghr[2] << "," << ghr[5] << ")";
    }
}

// Test different history lengths
TEST_F(FoldedHistTest, DifferentLengths) {
    // Test case where histLen < foldedLen
    FoldedHist hist1(4, 8, 2);
    boost::dynamic_bitset<> ghr1(4, 0);
    EXPECT_NO_THROW(hist1.update(ghr1, 1, true));
    
    // Test case where histLen = foldedLen
    FoldedHist hist2(8, 8, 2);
    boost::dynamic_bitset<> ghr2(8, 0);
    EXPECT_NO_THROW(hist2.update(ghr2, 1, true));
    
    // Test case where histLen > foldedLen
    FoldedHist hist3(16, 4, 2);
    boost::dynamic_bitset<> ghr3(16, 0);
    EXPECT_NO_THROW(hist3.update(ghr3, 1, true));
}

// Test maximum shift amount
TEST_F(FoldedHistTest, MaxShift) {
    FoldedHist hist(8, 4, 2);
    boost::dynamic_bitset<> ghr(8, 0);
    
    // Test shift amount equal to maxShamt
    EXPECT_NO_THROW(hist.update(ghr, 2, true));
    
    // Test shift amount less than maxShamt
    EXPECT_NO_THROW(hist.update(ghr, 1, true));
}

TEST_F(FoldedHistTest, BoundaryConditions) {
    // Test 1: histLen = 1
    {
        FoldedHist hist(1, 1, 1);
        boost::dynamic_bitset<> ghr(1, 1);
        EXPECT_NO_THROW(hist.update(ghr, 1, true));
    }
    
    // Test 2: foldedLen = 1
    {
        FoldedHist hist(8, 1, 1);
        boost::dynamic_bitset<> ghr(8, 0xFF);
        boost::dynamic_bitset<> temp_ghr(8, 0);
        for (int i = 7; i >= 0; i--) {
            temp_ghr <<= 1;
            if (i < 7) temp_ghr[0] = ghr[i+1];
            hist.update(temp_ghr, 1, ghr[i]);
        }
        EXPECT_NO_THROW(hist.check(ghr));
    }
}

TEST_F(FoldedHistTest, ChainedRecovery) {
    FoldedHist hist1(8, 4, 2);
    FoldedHist hist2(8, 4, 2);
    FoldedHist hist3(8, 4, 2);
    
    // Set up initial state
    boost::dynamic_bitset<> ghr(8);
    ghr[0] = 1; ghr[3] = 1; ghr[6] = 1;
    
    // Initialize hist1
    boost::dynamic_bitset<> temp_ghr(8, 0);
    for (int i = 7; i >= 0; i--) {
        temp_ghr <<= 1;
        if (i < 7) temp_ghr[0] = ghr[i+1];
        hist1.update(temp_ghr, 1, ghr[i]);
    }
    
    // Chain recoveries
    hist2.recover(hist1);
    hist3.recover(hist2);
    
    // All should be identical
    EXPECT_EQ(hist1.get(), hist2.get());
    EXPECT_EQ(hist2.get(), hist3.get());
    
    // All should pass check
    EXPECT_NO_THROW(hist1.check(ghr));
    EXPECT_NO_THROW(hist2.check(ghr));
    EXPECT_NO_THROW(hist3.check(ghr));
}
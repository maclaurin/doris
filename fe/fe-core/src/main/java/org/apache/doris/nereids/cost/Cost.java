// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package org.apache.doris.nereids.cost;

/**
 * Cost encapsulate the real cost with double type.
 * We do this because we want to customize the operation of adding child cost
 * according different operator
 */
public interface Cost {
    double getValue();

    /**
     * This is for calculating the cost in simplifier
     */
    static Cost withRowCount(double rowCount, boolean enableNewCostModel) {
        if (enableNewCostModel) {
            return new CostV2(0, rowCount, 0);
        }
        return new CostV1(rowCount);
    }

    /**
     * return zero cost
     */
    static Cost zero(boolean enableNewCostModel) {
        if (enableNewCostModel) {
            return CostV2.zero();
        }
        return CostV1.zero();
    }

    static Cost infinite(boolean enableNewCostModel) {
        if (enableNewCostModel) {
            return CostV2.infinite();
        }
        return CostV1.infinite();
    }
}


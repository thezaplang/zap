fun main() Int {
    var grades: [3][4]Int = {
        {5, 4, 3, 5}, 
        {3, 3, 4, 2},  
        {5, 5, 4, 4}   
    };

    var studentAvg: [3]Int = {0, 0, 0};
    var subjectAvg: [4]Int = {0, 0, 0, 0};

    
    var i: Int = 0;
    while i < 3 {
        var sum: Int = 0;

        var j: Int = 0;
        while j < 4 {
            sum = sum + grades[i][j];
            j = j + 1;
        }

        studentAvg[i] = sum / 4;
        i = i + 1;
    }

    
    var j: Int = 0;
    while j < 4 {
        var sum: Int = 0;

        var i2: Int = 0;
        while i2 < 3 {
            sum = sum + grades[i2][j];
            i2 = i2 + 1;
        }

        subjectAvg[j] = sum / 3;
        j = j + 1;
    }

   
    var bestIndex: Int = 0;
    var k: Int = 1;
    while k < 3 {
        if studentAvg[k] > studentAvg[bestIndex] {
            bestIndex = k;
        }
        k = k + 1;
    }

    
    // studentAvg = {4, 3, 4}
    // subjectAvg = {4, 4, 3, 3}
    // bestIndex = 0 (lub 2, remis)

    if studentAvg[0] == 4 &&
       studentAvg[1] == 3 &&
       studentAvg[2] == 4 &&
       subjectAvg[0] == 4 &&
       subjectAvg[1] == 4 &&
       subjectAvg[2] == 3 &&
       subjectAvg[3] == 3 &&
       bestIndex == 0 {
        return 0;
    }

    return 1;
}

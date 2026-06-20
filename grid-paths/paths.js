
// If all of my neighbors shortest paths to target go through me (or don't exist)
// then it is a closed path.

const kNorth = 1;
const kSouth = 2;
const kWest  = 4;
const kEast  = 8;

// http://www.lihaoyi.com/post/BuildyourownCommandLinewithANSIescapecodes.html
const kColorReset = "\u001b[0m";
const kColorRed   = "\u001b[31m";
const kColorBlack = "\u001b[30m";
const kColorGreen = "\u001b[32m";
const kColorYellow = "\u001b[33m";
const kColorBlue = "\u001b[34m";
const kColorMagenta = "\u001b[35m";
const kColorCyan = "\u001b[36m";
const kColorWhite = "\u001b[37m";
const kColorBrightRed   = "\u001b[31;1m";
const kColorBrightBlack = "\u001b[30;1m";
const kColorBrightGreen = "\u001b[32;1m";
const kColorBrightYellow = "\u001b[33;1m";
const kColorBrightBlue = "\u001b[34;1m";
const kColorBrightMagenta = "\u001b[35;1m";
const kColorBrightCyan = "\u001b[36;1m";
const kColorBrightWhite = "\u001b[37;1m";
const kColorBackgroundRed   = "\u001b[41m";
const kColorBackgroundBlack = "\u001b[40m";
const kColorBackgroundGreen = "\u001b[42m";
const kColorBackgroundYellow = "\u001b[43m";
const kColorBackgroundBlue = "\u001b[44m";
const kColorBackgroundMagenta = "\u001b[45m";
const kColorBackgroundCyan = "\u001b[46m";
const kColorBackgroundWhite = "\u001b[47m";
const kColorBackgroundBrightRed   = "\u001b[41;1m";
const kColorBackgroundBrightBlack = "\u001b[40;1m";
const kColorBackgroundBrightGreen = "\u001b[42;1m";
const kColorBackgroundBrightYellow = "\u001b[43;1m";
const kColorBackgroundBrightBlue = "\u001b[44;1m";
const kColorBackgroundBrightMagenta = "\u001b[45;1m";
const kColorBackgroundBrightCyan = "\u001b[46;1m";
const kColorBackgroundBrightWhite = "\u001b[47;1m";

const kColorGrid = kColorCyan;
const kColorObstacle = kColorWhite;
const kColorTarget = kColorMagenta;
const kColorSource = kColorYellow;
const kColorChanged = kColorGreen;

function CellIndex(grid,x,y) {
  return (y*grid.colCount)+x;
}

function ColorStr(str, color) {
  return color + str + kColorReset;
}

function CellPosition(grid,index) {
  var y = (index / grid.colCount)|0;
  var x = index - (y*grid.colCount)
  return { x: x, y: y };
}

var ValidDirections = function( grid, cellIndex ) {
  var pos = CellPosition(grid,cellIndex);
  var x = pos.x;
  var y = pos.y;

  var result = 0x0f;
  if (x == 0)
    result &= ~kWest;
  if (x == (grid.colCount-1))
    result &= ~kEast;
  if (y == 0)
    result &= ~kSouth;
  if (y == (grid.rowCount-1))
    result &= ~kNorth;

  if ((grid.obstacles[cellIndex] & kNorth) == kNorth)
    result &= ~kNorth;
  if ((grid.obstacles[cellIndex] & kSouth) == kSouth)
    result &= ~kSouth;
  if ((grid.obstacles[cellIndex] & kWest) == kWest)
    result &= ~kWest;
  if ((grid.obstacles[cellIndex] & kEast) == kEast)
    result &= ~kEast;

  return result;
} 

function InitializePaths(grid, x, y) {
  var targetCellIndex = CellIndex(grid,x,y);
  var paths = [];
  var sourceCellIndex;
  
  paths[targetCellIndex] = { dir: 0, dist: 0 };

  for (j=0;j<x;j++) {
    sourceCellIndex = CellIndex(grid,j,y);
    paths[sourceCellIndex] = { dir: kEast, dist: (x-j) };
  }
  for (j=x+1;j<grid.colCount;j++) {
    sourceCellIndex = CellIndex(grid,j,y);
    paths[sourceCellIndex] = { dir: kWest, dist: (j-x) };
  }
  for (i=0;i<y;i++) {
    sourceCellIndex = CellIndex(grid,x,i);
    paths[sourceCellIndex] = { dir: kNorth, dist: (y-i) };
  }
  for (i=y+1;i<grid.rowCount;i++) {
    sourceCellIndex = CellIndex(grid,x,i);
    paths[sourceCellIndex] = { dir: kSouth, dist: (i-y) };
  }

  for (i = 0;i<y;i++)
  for (j = 0;j<x;j++) {
    sourceCellIndex = CellIndex(grid,j,i);
    paths[sourceCellIndex] = { dir: kNorth | kEast, dist: (x-j)+(y-i) };
  }
  for (i = 0;i<y;i++)
  for (j = x+1;j<grid.colCount;j++) {
    sourceCellIndex = CellIndex(grid,j,i);
    paths[sourceCellIndex] = { dir: kNorth | kWest, dist: (j-x)+(y-i) };
  }
  for (i = y+1;i<grid.rowCount;i++)
  for (j = 0;j<x;j++) {
    sourceCellIndex = CellIndex(grid,j,i);
    paths[sourceCellIndex] = { dir: kSouth | kEast, dist: (x-j)+(i-y) };
  }
  for (i = y+1;i<grid.rowCount;i++)
  for (j = x+1;j<grid.colCount;j++) {
    sourceCellIndex = CellIndex(grid,j,i);
    paths[sourceCellIndex] = { dir: kSouth | kWest, dist: (j-x)+(i-y) };
  }

  grid.paths[targetCellIndex] = paths;
}

function InitializeGrid( colCount, rowCount ) {
  var grid = { colCount: colCount, rowCount: rowCount, paths: [], obstacles: [], island: [], islandCount: 1, islandTargetCounts: [ rowCount * colCount ] };
  for (var y=0;y<rowCount;y++)
  for (var x=0;x<colCount;x++) {
    var sourceCellIndex = CellIndex(grid,x,y);
    grid.obstacles[sourceCellIndex] = 0;
    grid.island[sourceCellIndex] = 0;
    InitializePaths(grid, x, y);
  }
  return grid;
}

function CellStr(cell) {
  if (!cell)
    return "?????   ";

  var dir = cell.dir;
  var dist= cell.dist;

  var dirStr = "";
  if ((dir & kNorth) == kNorth)
    dirStr += "N";
  if ((dir & kSouth) == kSouth)
    dirStr += "S";
  if ((dir & kWest) == kWest)
    dirStr += "W";
  if ((dir & kEast) == kEast)
    dirStr += "E";

  dirStr += dist;
  return "  " + dirStr.padEnd(6," ");
}
 

function PrintPaths( grid, config ) {
  var targetCellIndex = config.targetCellIndex;
  var highlightSourceCellIndex = config.sourceCellIndex;
  var highlightIslandIndex = config.highlightIslandIndex;
  var changedCellIndices = config.changedCellIndices || [];
  var targetPos = CellPosition(grid,targetCellIndex);
  var x = targetPos.x;
  var y = targetPos.y;

  var titleStr = "";
  titleStr += "Target: " + ColorStr( x + ", " + y, kColorTarget );
  if (highlightSourceCellIndex) {
    var sourcePos = CellPosition(grid,highlightSourceCellIndex);
    titleStr += " Source: " + ColorStr( sourcePos.x + ", " + sourcePos.y, kColorSource );
  }
  if (changedCellIndices.length > 0) {
    titleStr += ColorStr(" [Changed]",kColorChanged);
  }

  console.log("==== " + titleStr + " ====");

  var paths = grid.paths[targetCellIndex];
  var rowStr;
  for (var i=grid.rowCount-1;i>=0;i--) 
  {
    rowStr = ColorStr( "+", kColorGrid);
    for (var j=0;j<grid.colCount;j++) {
      sourceCellIndex = CellIndex(grid,j,i);
      if (i == (grid.rowCount-1))
        rowStr += ColorStr( "--------", kColorGrid);
      else if ((grid.obstacles[sourceCellIndex] & kNorth) == kNorth)
        rowStr += ColorStr( "--------", kColorObstacle);
      else
        rowStr += "        ";
      rowStr += ColorStr( "+", kColorGrid);
    }
    console.log(rowStr);

    rowStr = ColorStr( "|", kColorGrid );
    for (var j=0;j<grid.colCount;j++) {
      sourceCellIndex = CellIndex(grid,j,i);

      var changed = changedCellIndices.indexOf(sourceCellIndex) != -1;

      if ((i == y) && (j == x))
        rowStr += kColorTarget;
      if (changed) 
        rowStr += kColorChanged;
      if (sourceCellIndex == highlightSourceCellIndex)
        rowStr += kColorSource;
      if (grid.island[sourceCellIndex] == highlightIslandIndex)
        rowStr += kColorSource;

      rowStr += CellStr(paths[sourceCellIndex]);

      if (sourceCellIndex == highlightSourceCellIndex)
        rowStr += kColorReset;
      if ((i == y) && (j == x))
        rowStr += kColorReset;
      if (changed) 
        rowStr += kColorReset;
      if (grid.island[sourceCellIndex] == highlightIslandIndex)
        rowStr += kColorReset;

      if (j == (grid.colCount-1))
        rowStr += ColorStr( "|", kColorGrid);
      else if ((grid.obstacles[sourceCellIndex] & kEast) == kEast)
        rowStr += ColorStr( "|", kColorObstacle);
      else
        rowStr += " ";
    }
    console.log(rowStr);

  }
  rowStr = kColorGrid + "+";
  for (var j=0;j<grid.colCount;j++) {
    rowStr += "--------";
    rowStr += "+";
  }
  rowStr += kColorReset;
  console.log(rowStr);
}

function FindBlockedTargets( grid, x, y, wall ) {
  var targets = [];
  var sourceCellIndex = CellIndex(grid,x,y);

  for (var i=0;i<grid.rowCount;i++)
  for (var j=0;j<grid.colCount;j++) {
    var targetCellIndex = CellIndex(grid,j,i);
    var paths = grid.paths[targetCellIndex];
    var cell = paths[sourceCellIndex];
    if (wall == kNorth) {
      if ((cell.dir & kNorth) == kNorth)
        targets.push(targetCellIndex); 
    } else if (wall == kSouth) {
      if ((cell.dir & kSouth) == kSouth)
        targets.push(targetCellIndex); 
    } else if (wall == kWest) {
      if ((cell.dir & kWest) == kWest)
        targets.push(targetCellIndex); 
    } else if (wall == kEast) {
      if ((cell.dir & kEast) == kEast)
        targets.push(targetCellIndex); 
    }
  }
  return targets;
}

function AddObstacleToGrid( grid, cellIndex, dir ) {
  var cellIndex0 = cellIndex;
  var pos = CellPosition( grid, cellIndex0 );
  var x = pos.x;
  var y = pos.y;

  if (dir == kNorth) {
    grid.obstacles[cellIndex0] |= kNorth;
    if (y < (grid.rowCount-1)) {
      var cellIndex1 = CellIndex(grid,x,y+1);
      grid.obstacles[cellIndex1] |= kSouth;
    }
  }
  if (dir == kSouth) {
    grid.obstacles[cellIndex0] |= kSouth;
    if (y > 0) {
      var cellIndex1 = CellIndex(grid,x,y-1);
      grid.obstacles[cellIndex1] |= kNorth;
    }
  }
  if (dir == kEast) {
    grid.obstacles[cellIndex0] |= kEast;
    if (x < (grid.colCount-1)) {
      var cellIndex1 = CellIndex(grid,x+1,y);
      grid.obstacles[cellIndex1] |= kWest;
    }
  }
  if (dir == kWest) {
    var cellIndex0 = CellIndex(grid,x,y);
    grid.obstacles[cellIndex0] |= kWest;
    if (x > 0) {
      var cellIndex1 = CellIndex(grid,x-1,y);
      grid.obstacles[cellIndex1] |= kEast;
    }
  }
}

function AddObstaclePass1( grid, x, y, dir ) {
  var resultBlockedTargets = [];

  var sourceCellIndex0 = CellIndex( grid, x, y );
  var blockedDirection0 = dir;

  AddObstacleToGrid( grid, sourceCellIndex0, dir );
  var blockedTargets0 = FindBlockedTargets( grid, x, y, dir);

  blockedTargets0.forEach( function( targetCellIndex) {
    resultBlockedTargets.push( {
      targetCellIndex: targetCellIndex,
      sourceCellIndex: sourceCellIndex0,
      blockedDirection: blockedDirection0
    });
  });


  console.log("===========================================================");
  console.log("==== Find shortest paths that go through obstacle.     ====");
  console.log("==== - Start with the source cell whose best path goes ====");
  console.log("====   the obstacle in one direction.                  ====");
  console.log("===========================================================");
  for (var i=0;i<blockedTargets0.length;i++) {
    PrintPaths(grid, {
      targetCellIndex: blockedTargets0[i],
      sourceCellIndex: sourceCellIndex0
    });
  }

  var sourceCellIndex1 = 0;
  var blockedTargets1 = [];
  if ((dir == kNorth) && (y < (grid.rowCount-1))) {
    sourceCellIndex1 = CellIndex( grid, x, y+1 );
    blockedTargets1 = FindBlockedTargets( grid, x, y+1, kSouth);
    blockedDirection1 = kSouth;
  } 
  if ((dir == kSouth) && (y > 0)) {
    sourceCellIndex1 = CellIndex( grid, x, y-1 );
    blockedTargets1 = FindBlockedTargets( grid, x, y-1, kNorth);
    blockedDirection1 = kNorth;
  } 
  if ((dir == kEast) && (x < (grid.colCount-1))) {
    sourceCellIndex1 = CellIndex( grid, x+1, y );
    blockedTargets1 = FindBlockedTargets( grid, x+1, y, kWest);
    blockedDirection1 = kWest;
  } 
  if ((dir == kWest) && (x > 0)) {
    sourceCellIndex1 = CellIndex( grid, x-1, y );
    blockedTargets1 = FindBlockedTargets( grid, x-1, y, kEast);
    blockedDirection1 = kEast;
  } 

  if (blockedTargets1.length > 0) {
    blockedTargets1.forEach( function( targetCellIndex) {
      resultBlockedTargets.push( {
        targetCellIndex: targetCellIndex,
        sourceCellIndex: sourceCellIndex1,
        blockedDirection: blockedDirection1
      });
    });

    console.log("===========================================================");
    console.log("==== Find shortest paths that go through obstacle.     ====");
    console.log("==== - Start with the source cell whose best path goes ====");
    console.log("====   the obstacle in one direction.                  ====");
    console.log("==== - Next check the source cell whose best path goes ====");
    console.log("====   the obstacle in opposite direction.             ====");
    console.log("===========================================================");
    for (var i=0;i<blockedTargets1.length;i++) {
      PrintPaths(grid, {
        targetCellIndex: blockedTargets1[i],
        sourceCellIndex: sourceCellIndex1
      });
    }
  }

  return resultBlockedTargets;
}

function AddObstaclePass2( grid, x, y, dir ) {
  var blockedTargets = AddObstaclePass1( grid, x, y, dir );
  var fixupBlockedTargets = [];

  console.log("===========================================================");
  console.log("==== Find shortest paths that go through obstacle.     ====");
  console.log("==== - Start with the source cell whose best path goes ====");
  console.log("====   the obstacle in one direction.                  ====");
  console.log("==== - Next check the source cell whose best path goes ====");
  console.log("====   the obstacle in opposite direction.             ====");
  console.log("==== - Remove blocked paths that have alternative      ====");
  console.log("====   shotest path.                                   ====");
  console.log("===========================================================");

  // { targetCellIndex: 
  //   sourceCellIndex: 
  //   blockedDirection: }

  blockedTargets.forEach( function( blockedTarget ) { 
    var targetCellIndex = blockedTarget.targetCellIndex;
    var sourceCellIndex = blockedTarget.sourceCellIndex;
    var blockedDirection = blockedTarget.blockedDirection;
    var targetPaths = grid.paths[targetCellIndex];
    var hasAlternativePath = (targetPaths[sourceCellIndex].dir & (~blockedDirection)) != 0;

    if (hasAlternativePath) {  
      targetPaths[sourceCellIndex].dir &= ~blockedDirection;

      PrintPaths(grid, {
        targetCellIndex: targetCellIndex,
        sourceCellIndex: sourceCellIndex
      });

    } else {
      fixupBlockedTargets.push( blockedTarget );
    }
  });

  return fixupBlockedTargets;
}

function DivideIsland( grid, blockedTargets ) {
  var islandIndex = grid.islandCount;
  grid.islandCount += 2;
  var firstDirection = blockedTargets[0].blockedDirection;

  grid.islandTargetCounts[islandIndex] = 0;
  grid.islandTargetCounts[islandIndex+1] = 0;

  blockedTargets.forEach( function( target ) {
    var targetCellIndex = target.targetCellIndex;
    var targetDirection = target.blockedDirection;

    if ( targetDirection == firstDirection ) {
      grid.island[ targetCellIndex ] = islandIndex;
      grid.islandTargetCounts[islandIndex]++;
    } else {
      grid.island[ targetCellIndex ] = islandIndex+1;
      grid.islandTargetCounts[islandIndex+1]++;
    }
  });

  blockedTargets.forEach( function( target ) {
    var targetCellIndex = target.targetCellIndex;
    var targetDirection = target.blockedDirection;
    var targetPaths = grid.paths[targetCellIndex];
 
    blockedTargets.forEach( function( source ) {
      var sourceCellIndex = source.targetCellIndex;
      var sourceDirection = source.blockedDirection;

      if (targetDirection != sourceDirection) {
        targetPaths[sourceCellIndex].dir = 0;
        targetPaths[sourceCellIndex].dist = -1;
      }
    });

    console.log("===========================================================");
    console.log("==== Path blocked due to closed island.                ====");
    console.log("===========================================================");

    var highlightIslandIndex;
    if ( targetDirection == firstDirection ) {
      highlightIslandIndex = islandIndex + 1;
    } else {
      highlightIslandIndex = islandIndex;
    }

    PrintPaths(grid, {
      targetCellIndex: targetCellIndex,
      highlightIslandIndex: highlightIslandIndex
    });
  });
}

function AddObstacle( grid, x, y, dir ) {
  var islandIndex = grid.island[ CellIndex(grid,x,y) ];
  var islandTargetCount = grid.islandTargetCounts[islandIndex];
  var blockedTargets = AddObstaclePass2( grid, x, y, dir );

  // console.log("blockedTargets.length: " + blockedTargets.length);
  if (blockedTargets.length == islandTargetCount) {
    DivideIsland( grid, blockedTargets );
    return;
  }

  console.log("===========================================================");
  console.log("==== Find shortest paths that go through obstacle.     ====");
  console.log("==== - Start with the source cell whose best path goes ====");
  console.log("====   the obstacle in one direction.                  ====");
  console.log("==== - Next check the source cell whose best path goes ====");
  console.log("====   the obstacle in opposite direction.             ====");
  console.log("==== - Remove blocked paths that have alternative      ====");
  console.log("====   shotest path.                                   ====");
  console.log("==== - Remaining blocked paths have no known path.     ====");
  console.log("===========================================================");

  blockedTargets.forEach( function( blockedTarget ) { 
    var targetCellIndex = blockedTarget.targetCellIndex;
    var sourceCellIndex = blockedTarget.sourceCellIndex;
    var blockedDirection = blockedTarget.blockedDirection;
    var targetPaths = grid.paths[targetCellIndex];

    targetPaths[sourceCellIndex].dir &= ~blockedDirection;
    targetPaths[sourceCellIndex].dist = -1;

    PrintPaths(grid, {
      targetCellIndex: targetCellIndex,
      sourceCellIndex: sourceCellIndex
   });
  });

  console.log("===========================================================");
  console.log("==== Find shortest paths that go through obstacle.     ====");
  console.log("==== - Start with the source cell whose best path goes ====");
  console.log("====   the obstacle in one direction.                  ====");
  console.log("==== - Next check the source cell whose best path goes ====");
  console.log("====   the obstacle in opposite direction.             ====");
  console.log("==== - Remove blocked paths that have alternative      ====");
  console.log("====   shotest path.                                   ====");
  console.log("==== - Remaining blocked paths have no known path.     ====");
  console.log("==== - Recursively find new path. No need to solve for ====");
  console.log("====   full path; searching for shortest path to       ====");
  console.log("====   existing, valid shortest path.                  ====");
  console.log("===========================================================");

  blockedTargets.forEach( function( blockedTarget ) { 
    var targetCellIndex = blockedTarget.targetCellIndex;
    var sourceCellIndex = blockedTarget.sourceCellIndex;
    var blockedDirection = blockedTarget.blockedDirection;
    var targetPaths = grid.paths[targetCellIndex];
    var changedCellIndices = [];

    ResolveBlockedPath( grid, sourceCellIndex, targetCellIndex, [], changedCellIndices );
    ResolveChangedPaths( grid, targetCellIndex, changedCellIndices );

    PrintPaths(grid, {
      targetCellIndex: targetCellIndex,
      sourceCellIndex: sourceCellIndex,
      changedCellIndices: changedCellIndices
   });

  });

}

function ResolveChangedPaths( grid, targetCellIndex, changedCellIndices ) {
  var targetPaths = grid.paths[targetCellIndex];
  var changedCount = 0;
  changedCellIndices.forEach( function( sourceCellIndex ) {
    var sourceCellPos = CellPosition(grid, sourceCellIndex);
    var x = sourceCellPos.x;
    var y = sourceCellPos.y;
    var neighborNCellIndex = CellIndex(grid, x, y+1 );
    var neighborSCellIndex = CellIndex(grid, x, y-1 );
    var neighborECellIndex = CellIndex(grid, x+1, y );
    var neighborWCellIndex = CellIndex(grid, x-1, y );
    var validDirections = ValidDirections( grid, sourceCellIndex );

    var bestDist = grid.rowCount * grid.colCount;
    var pathDir = 0;
  
    if (( validDirections & kNorth ) == kNorth) {
      if (CellHasPath( grid, neighborNCellIndex, targetCellIndex )) {
        if (targetPaths[neighborNCellIndex].dist < bestDist) {
          bestDist = targetPaths[neighborNCellIndex].dist;
        }
      }
    }
    if (( validDirections & kSouth ) == kSouth ) {
      if (CellHasPath( grid, neighborSCellIndex, targetCellIndex )) {
        if (targetPaths[neighborSCellIndex].dist < bestDist) {
          bestDist = targetPaths[neighborSCellIndex].dist;
        }
      }
    }
    if (( validDirections & kEast ) == kEast ) {
      if (CellHasPath( grid, neighborECellIndex, targetCellIndex )) {
        if (targetPaths[neighborECellIndex].dist < bestDist) {
          bestDist = targetPaths[neighborECellIndex].dist;
        }
      }
    }
    if (( validDirections & kWest ) == kWest ) {
      if (CellHasPath( grid, neighborWCellIndex, targetCellIndex )) {
        if (targetPaths[neighborWCellIndex].dist < bestDist) {
          bestDist = targetPaths[neighborWCellIndex].dist;
        }
      }
    }
  
    if (( validDirections & kNorth ) == kNorth) {
      if (CellHasPath( grid, neighborNCellIndex, targetCellIndex )) {
        if (targetPaths[neighborNCellIndex].dist <= bestDist) {
          pathDir |= kNorth;
        }
      }
    }
    if (( validDirections & kSouth ) == kSouth ) {
      if (CellHasPath( grid, neighborSCellIndex, targetCellIndex )) {
        if (targetPaths[neighborSCellIndex].dist <= bestDist) {
          pathDir |= kSouth;
        }
      }
    }
    if (( validDirections & kEast ) == kEast ) {
      if (CellHasPath( grid, neighborECellIndex, targetCellIndex )) {
        if (targetPaths[neighborECellIndex].dist <= bestDist) {
          pathDir |= kEast;
        }
      }
    }
    if (( validDirections & kWest ) == kWest ) {
      if (CellHasPath( grid, neighborWCellIndex, targetCellIndex )) {
        if (targetPaths[neighborWCellIndex].dist <= bestDist) {
          pathDir |= kWest;
        }
      }
    }
  
    var pathDist = bestDist+1;
    if (( targetPaths[sourceCellIndex].dist != pathDist ) || 
        ( targetPaths[sourceCellIndex].dir != pathDir)) {
      targetPaths[sourceCellIndex].dist = pathDist;
      targetPaths[sourceCellIndex].dir = pathDir;
      changedCount++;
    }
  });

  return changedCount;
}

function CellHasPath( grid, sourceCellIndex, targetCellIndex ) {
  var targetPaths = grid.paths[targetCellIndex];
  return targetPaths[sourceCellIndex].dist != -1;
}

function ResolveBlockedPath( grid, sourceCellIndex, targetCellIndex, parentCellIndices, changedCellIndices ) {
  var sourceCellPos = CellPosition(grid, sourceCellIndex);
  var x = sourceCellPos.x;
  var y = sourceCellPos.y;
  var targetPaths = grid.paths[targetCellIndex];
  var pathDir = targetPaths[sourceCellIndex].dir;
  var validDirections = ValidDirections( grid, sourceCellIndex );

  if (validDirections == 0) {
    targetPaths[sourceCellIndex].dist = -1;
    targetPaths[sourceCellIndex].dir = 0;
    return;
  }

  pathDir &= validDirections;

  // console.log( "Resolve: " + JSON.stringify(sourceCellPos) + " " + JSON.stringify(parentCellIndices) + " " + pathDir + " validDir: " + validDirections + " obst: " + grid.obstacles[sourceCellIndex]  );

  // Remove invalid paths
  if (( pathDir & kNorth ) == kNorth) {
    if (!CellHasPath( grid, CellIndex(grid, x, y+1), targetCellIndex )) {
      pathDir &= ~kNorth;
    }
  }
  if (( pathDir & kSouth ) == kSouth ) {
    if (!CellHasPath( grid, CellIndex(grid, x, y-1), targetCellIndex )) {
      pathDir &= ~kSouth;
    }
  }
  if (( pathDir & kEast ) == kEast ) {
    if (!CellHasPath( grid, CellIndex(grid, x+1, y), targetCellIndex )) {
      pathDir &= ~kEast;
    }
  }
  if (( pathDir & kWest ) == kWest ) {
    if (!CellHasPath( grid, CellIndex(grid, x-1, y), targetCellIndex )) {
      pathDir &= ~kWest;
    }
  }

  // no changes, return
  if ((pathDir != 0) && (targetPaths[sourceCellIndex].dir == pathDir))
    return;

  if (changedCellIndices.indexOf(sourceCellIndex) == -1)
    changedCellIndices.push(sourceCellIndex);

  targetPaths[sourceCellIndex].dir = pathDir;

  // console.log( validDirections + " - " + pathDir + " = " + targetPaths[sourceCellIndex].dir );

  // has remaining, return.
  if (pathDir != 0)
    return;

  targetPaths[sourceCellIndex].dist = -1;

  parentCellIndices.push( sourceCellIndex );

  var neighborNCellIndex = CellIndex(grid, x, y+1 );
  var neighborSCellIndex = CellIndex(grid, x, y-1 );
  var neighborECellIndex = CellIndex(grid, x+1, y );
  var neighborWCellIndex = CellIndex(grid, x-1, y );

  // #todo check parent cell index

  // for each neighbor with valid path
  // recurse
  if (parentCellIndices.indexOf(neighborNCellIndex) == -1)
  if (( validDirections & kNorth ) == kNorth) {
    if (CellHasPath( grid, neighborNCellIndex, targetCellIndex )) {
      ResolveBlockedPath( grid, neighborNCellIndex, targetCellIndex, parentCellIndices, changedCellIndices );
    }
  }

  if (parentCellIndices.indexOf(neighborSCellIndex) == -1)
  if (( validDirections & kSouth ) == kSouth ) {
    if (CellHasPath( grid, neighborSCellIndex, targetCellIndex )) {
      ResolveBlockedPath( grid, neighborSCellIndex, targetCellIndex, parentCellIndices, changedCellIndices );
    }
  }

  if (parentCellIndices.indexOf(neighborECellIndex) == -1)
  if (( validDirections & kEast ) == kEast ) {
    if (CellHasPath( grid, neighborECellIndex, targetCellIndex )) {
      ResolveBlockedPath( grid, neighborECellIndex, targetCellIndex, parentCellIndices, changedCellIndices );
    }
  }

  if (parentCellIndices.indexOf(neighborWCellIndex) == -1)
  if (( validDirections & kWest ) == kWest ) {
    if (CellHasPath( grid, neighborWCellIndex, targetCellIndex )) {
      ResolveBlockedPath( grid, neighborWCellIndex, targetCellIndex, parentCellIndices, changedCellIndices );
    }
  }

  // select best of neighbor paths

  var bestDist = grid.rowCount * grid.colCount;

  if (( validDirections & kNorth ) == kNorth) {
    if (CellHasPath( grid, neighborNCellIndex, targetCellIndex )) {
      if (targetPaths[neighborNCellIndex].dist < bestDist) {
        bestDist = targetPaths[neighborNCellIndex].dist;
      }
    }
  }
  if (( validDirections & kSouth ) == kSouth ) {
    if (CellHasPath( grid, neighborSCellIndex, targetCellIndex )) {
      if (targetPaths[neighborSCellIndex].dist < bestDist) {
        bestDist = targetPaths[neighborSCellIndex].dist;
      }
    }
  }
  if (( validDirections & kEast ) == kEast ) {
    if (CellHasPath( grid, neighborECellIndex, targetCellIndex )) {
      if (targetPaths[neighborECellIndex].dist < bestDist) {
        bestDist = targetPaths[neighborECellIndex].dist;
      }
    }
  }
  if (( validDirections & kWest ) == kWest ) {
    if (CellHasPath( grid, neighborWCellIndex, targetCellIndex )) {
      if (targetPaths[neighborWCellIndex].dist < bestDist) {
        bestDist = targetPaths[neighborWCellIndex].dist;
      }
    }
  }

  pathDir = 0;

  if (( validDirections & kNorth ) == kNorth) {
    if (CellHasPath( grid, neighborNCellIndex, targetCellIndex )) {
      if (targetPaths[neighborNCellIndex].dist <= bestDist) {
        pathDir |= kNorth;
      }
    }
  }
  if (( validDirections & kSouth ) == kSouth ) {
    if (CellHasPath( grid, neighborSCellIndex, targetCellIndex )) {
      if (targetPaths[neighborSCellIndex].dist <= bestDist) {
        pathDir |= kSouth;
      }
    }
  }
  if (( validDirections & kEast ) == kEast ) {
    if (CellHasPath( grid, neighborECellIndex, targetCellIndex )) {
      if (targetPaths[neighborECellIndex].dist <= bestDist) {
        pathDir |= kEast;
      }
    }
  }
  if (( validDirections & kWest ) == kWest ) {
    if (CellHasPath( grid, neighborWCellIndex, targetCellIndex )) {
      if (targetPaths[neighborWCellIndex].dist <= bestDist) {
        pathDir |= kWest;
      }
    }
  }

  var pathDist = bestDist+1;
  targetPaths[sourceCellIndex].dist = pathDist;
  targetPaths[sourceCellIndex].dir = pathDir;

  // Patch in neighbors direction if this direction is smaller.

/*
  if (( validDirections & kNorth ) == kNorth) {
    if (CellHasPath( grid, neighborNCellIndex, targetCellIndex )) {
      if (targetPaths[neighborNCellIndex].dist > pathDist) {
        targetPaths[neighborNCellIndex].dir |= kSouth;
        ResolveBlockedPath( grid, neighborNCellIndex, targetCellIndex, parentCellIndices, changedCellIndices );
      }
    }
  }
  if (( validDirections & kSouth ) == kSouth ) {
    if (CellHasPath( grid, neighborSCellIndex, targetCellIndex )) {
      if (targetPaths[neighborSCellIndex].dist > pathDist) {
        targetPaths[neighborSCellIndex].dir |= kNorth;
        ResolveBlockedPath( grid, neighborSCellIndex, targetCellIndex, parentCellIndices, changedCellIndices );
      }
    }
  }
  if (( validDirections & kEast ) == kEast ) {
    if (CellHasPath( grid, neighborECellIndex, targetCellIndex )) {
      if (targetPaths[neighborECellIndex].dist > pathDist) {
        targetPaths[neighborECellIndex].dir |= kWest;
        ResolveBlockedPath( grid, neighborECellIndex, targetCellIndex, parentCellIndices, changedCellIndices );
      }
    }
  }
  if (( validDirections & kWest ) == kWest ) {
    if (CellHasPath( grid, neighborWCellIndex, targetCellIndex )) {
      if (targetPaths[neighborWCellIndex].dist > pathDist) {
        targetPaths[neighborWCellIndex].dir |= kEast;
        ResolveBlockedPath( grid, neighborWCellIndex, targetCellIndex, parentCellIndices, changedCellIndices );
      }
    }
  }
*/

  parentCellIndices.pop();
}

var grid = InitializeGrid(8,8);
AddObstacle(grid, 4, 4, kNorth);
AddObstacle(grid, 4, 4, kWest);
AddObstacle(grid, 5, 4, kWest);
// AddObstacle(grid, 4, 4, kSouth);
AddObstacle(grid, 5, 3, kWest);
AddObstacle(grid, 4, 3, kSouth);
AddObstacle(grid, 4, 3, kWest);

AddObstacle(grid, 7, 4, kNorth);
AddObstacle(grid, 7, 4, kWest);
AddObstacle(grid, 8, 4, kWest);
AddObstacle(grid, 7, 3, kSouth);
AddObstacle(grid, 7, 3, kWest);


